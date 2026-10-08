// JPOV Fire-Fog — GLSL 着色器（点状雾 MVP）
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md。本文件是**一次全屏 pass** 的 shader：
// 逐像素重建视线 → 屏幕 tile 剪枝（L1，只取本 tile 命中的 ≤K 个团）→ 每团沿球弦按 M 段
// 采样（L2，段内抖动）→ ZDist 累加（τd/Ed 可加）→ 降维到 ≤8 控制点（二叉最小堆贪心）→
// 末端积分 → **就地** alpha 混合到当前绑定的 3D HDR FBO。
//
// 合成：本 pass 不采样场景颜色，靠固定管线混合与帧缓冲里**已有**的颜色合成：
//   FragColor = vec4(S, T)（S=源累积亮度，T=总透射率）
//   glBlendFunc(GL_ONE, GL_SRC_ALPHA) ⇒ out.rgb = S + dst.rgb·T = L_out
//   （dst = 场景颜色 L_scene；无雾像素 S=0,T=1 ⇒ out=dst，恒等）
//   这才是设计 §3 的 L_out = L_scene·T + S，无需自持 FBO、无需采样场景颜色。
//
// MVP 边界（经 Danis 2026-10-08 确认）：
//   - 采样器只有 kAnalyticProfile（点状雾）；光照**不采样**任何 sun/shadow/ambient，
//     L_in = 每团常量发射色（color），消光 σ = intensity·profile(r)；
//   - M = 每团 2 段，K = 每 tile 8 团，tile = 16×16；
//   - 深度裁剪：用 MRT#1 场景深度把雾段裁到可见面前；
//   - 抖动：屏幕坐标哈希（确定性 ⇒ 可 gold）；
//   - 降维：二叉最小堆贪心（对应 CPU zdist_function.h 的 GreedyReduceFast）；
//   - 不做：屏幕空间深度敏感高斯（L3）、独立雾纹理/合成 pass（后续）。
//
// ⚠️ shader 里的 #define 常量必须与 fire_fog_renderer.h 的常量一一对应（改一处必须同步）。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_

namespace jpov {

// 全屏三角形 VS（gl_VertexID 覆盖 NDC，无 VAO/VBO；与 horizon_fog 同款）。
inline constexpr const char* kFireFogVs = R"glsl(
#version 330 core
out vec2 vTexCoord;
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kFireFogFs = R"glsl(
#version 330 core

// ── tile culling 常量（与 fire_fog_renderer.h 对应：改一处必须同步）──
#define TILE_SIZE           16
#define MAX_FOGS_PER_TILE    8
#define TEXELS_PER_TILE      2     // = MAX_FOGS_PER_TILE / 4（每 texel RGBA 存 4 个 uint8 索引）
#define FOG_INDEX_SENTINEL   255u

// ── ZDist 常量（与 zdist_function.h 对应）──
#define ZDIST_K              8     // 控制点上限
#define SEG_PER_FOG          2     // M：每团 z 段数
#define ZC                  40     // 候选断点上限（K*M*2+2 = 34，留裕量）
#define ZC1                 41
#define HEAP_CAP           128     // 堆容量（初始 m-2 + 每次移除 ≤2 更新）

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uSceneDepthTex;    // MRT#1 场景深度（R32F；gl_FragCoord.z，1.0=远平面）
uniform sampler2D uTileFogIndices;   // tile → 团索引（RGBA8；每 tile TEXELS_PER_TILE 个 texel）
uniform sampler2D uFogBodyTex;       // 团属性（RGBA32F；每团 3 texel）
uniform mat4  uInvVP;                // 相机 逆(Proj*View)
uniform vec3  uCamPos;               // 相机世界位置
uniform float uZNear;                // 相机近平面（米）
uniform float uZFar;                 // 相机远平面（米）
uniform int   uTotalFogs;            // 本帧团总数（团属性纹理有效行数）

// ═══════════════════════════ §0 通用工具 ═══════════════════════════

// 整数混洗哈希（wang-ish，确定性；不含 trigonometric 以免驱动差异）。
uint HashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 屏幕坐标哈希抖动 ∈ [0,1)：seed = (像素 x, 像素 y, 类序号)。
float Hash01(uvec3 s) {
    uint h = HashU(s.x + s.y * 0x9e3779b9u + s.z * 0x85ebca6bu);
    return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

// 径向消光剖面：u = r/R ∈ [0,1] → 权重 w ∈ [0,1]（对应 render_command.h 的 FogAttenuation）。
float ProfileAtten(int kind, float u) {
    if (u >= 1.0) {
        return 0.0;                     // 球外不贡献
    }
    u = clamp(u, 0.0, 1.0);
    if (kind == 0) {
        return 1.0;                     // kUniform
    }
    if (kind == 1) {
        return 1.0 - u;                 // kLinear
    }
    if (kind == 2) {
        return 1.0 - u * u;             // kQuadratic
    }
    const float k = 4.0;                // kExponential（归一化到边缘为 0）
    return (exp(-k * u) - exp(-k)) / (1.0 - exp(-k));
}

// ═══════════════════════════ §1 ZDist 累加（stage 1，精确）═══════════════════════════
//
// 逐段把 (Δτ, ΔEd) 以**加法**叠进 (τd, Ed)：段的贡献 = 断点 z0,z1 之间一条 ramp。
// 先插入断点（用**当前**函数值），再对所有断点加 ramp ⇒ 各 ramp 之和 ⇒ 乱序可加、免排序。
// 与 CPU zdist_function.h 的 ZDistAccumulator 逐式一致。

// 线性插值求值（域外按端点夹断）。
float ZInterpTau(float zs[ZC], float taus[ZC], int m, float z) {
    if (z <= zs[0]) {
        return taus[0];
    }
    if (z >= zs[m - 1]) {
        return taus[m - 1];
    }
    int i = 0;
    while (i + 2 < m && z > zs[i + 1]) {
        ++i;
    }
    float t = (z - zs[i]) / (zs[i + 1] - zs[i]);
    return taus[i] + (taus[i + 1] - taus[i]) * t;
}
vec3 ZInterpEd(float zs[ZC], vec3 eds[ZC], int m, float z) {
    if (z <= zs[0]) {
        return eds[0];
    }
    if (z >= zs[m - 1]) {
        return eds[m - 1];
    }
    int i = 0;
    while (i + 2 < m && z > zs[i + 1]) {
        ++i;
    }
    float t = (z - zs[i]) / (zs[i + 1] - zs[i]);
    return eds[i] + (eds[i + 1] - eds[i]) * t;
}

// 保证 x 是断点（缺失则用当前精确函数值插入，保持升序、无重复）。
void ZInsertBreakpoint(inout float zs[ZC], inout float taus[ZC], inout vec3 eds[ZC],
                       inout int m, float x) {
    int pos = 0;
    while (pos < m && zs[pos] < x) {
        ++pos;
    }
    if (pos < m && zs[pos] == x) {
        return;                         // 已是断点
    }
    float cur_tau = ZInterpTau(zs, taus, m, x);
    vec3  cur_ed  = ZInterpEd(zs, eds, m, x);
    for (int i = m; i > pos; --i) {
        zs[i] = zs[i - 1];
        taus[i] = taus[i - 1];
        eds[i] = eds[i - 1];
    }
    zs[pos] = x;
    taus[pos] = cur_tau;
    eds[pos] = cur_ed;
    ++m;
}

// 叠加一个 z 段 [z0,z1]（消光 sigma、内散射色 Lin）。超出域 [zs[0],zs[m-1]] 的部分裁掉。
void ZAddSegment(inout float zs[ZC], inout float taus[ZC], inout vec3 eds[ZC],
                 inout int m, float z0, float z1, float sigma, vec3 Lin) {
    float c0 = max(z0, zs[0]);
    float c1 = min(z1, zs[m - 1]);
    if (c1 <= c0) {
        return;                         // 与域无交叠
    }
    float len = c1 - c0;
    float rise_tau = sigma * len;
    vec3  rise_ed  = Lin * (sigma * len);
    ZInsertBreakpoint(zs, taus, eds, m, c0);
    ZInsertBreakpoint(zs, taus, eds, m, c1);
    for (int i = 0; i < m; ++i) {
        float f = clamp((zs[i] - c0) / (c1 - c0), 0.0, 1.0);   // ramp 因子
        taus[i] += f * rise_tau;
        eds[i]  += rise_ed * f;
    }
}

// ═══════════════════════════ §2 ZDist 降维（stage 2，二叉最小堆贪心）═══════════════════════════
//
// 反复合并“增量误差最小”的内部相邻点，直到剩 ZDIST_K 个。与 CPU GreedyReduceFast 一致：
// 代价用前缀和 O(1)（下式与 zdist_function.h 的 PrefixCost 逐式相同），堆做 O(m log m)。
// 端点恒保留 ⇒ τ_total/Ed_total 守恒。

// 候选点 i 的通道取值：c=0 → τd；c=1/2/3 → Ed.r/g/b。
float ChanVal(float taus[ZC], vec3 eds[ZC], int i, int c) {
    if (c == 0) {
        return taus[i];
    }
    if (c == 1) {
        return eds[i].r;
    }
    if (c == 2) {
        return eds[i].g;
    }
    return eds[i].b;
}

// 弦 vs 真折线、以 exp(−τd) 加权的四通道 L2 代价（O(1)；系数来自前缀和）。
// 注：GLSL 3.30 不支持数组的数组，四通道前缀和用一维数组展平：index = c*ZC1 + k。
float ZCost(float zs[ZC], float taus[ZC], vec3 eds[ZC],
            float p0[ZC1], float pz[ZC1], float pzz[ZC1],
            float py[4 * ZC1], float pzy[4 * ZC1], float pyy[4 * ZC1],
            int p, int q) {
    float zp = zs[p];
    float zq = zs[q];
    float P0  = p0[q]  - p0[p];
    float Pz  = pz[q]  - pz[p];
    float Pzz = pzz[q] - pzz[p];
    float total = 0.0;
    for (int c = 0; c < 4; ++c) {
        float yp = ChanVal(taus, eds, p, c);
        float yq = ChanVal(taus, eds, q, c);
        float s = (yq - yp) / (zq - zp);
        float a = yp - s * zp;
        float Py  = py[c * ZC1 + q]  - py[c * ZC1 + p];
        float Pzy = pzy[c * ZC1 + q] - pzy[c * ZC1 + p];
        float Pyy = pyy[c * ZC1 + q] - pyy[c * ZC1 + p];
        total += 3.0 * P0 * a * a + 3.0 * Pz * a * s + Pzz * s * s
                 - 3.0 * Py * a - Pzy * s + Pyy;
    }
    return total;
}

void main() {
    // ═══════════════════════════ §1 视线重建 ═══════════════════════════
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 ro = uCamPos;
    vec3 rd = normalize(pf.xyz / pf.w - ro);      // 单位方向 ⇒ 光线参数 t = 米

    // ═══════════════════════════ §2 场景深度（线性视深）═══════════════════════════
    float dndc = texture(uSceneDepthTex, vTexCoord).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    float scene_t = length(pw.xyz / pw.w - ro);   // 相机到可见面的距离（米）

    // ZDist 域 = [z_near, min(z_far, scene_t)]（深度裁剪：雾只累加到可见面前）。
    float z_near = uZNear;
    float z_far = min(uZFar, scene_t);
    if (z_far <= z_near + 1e-6) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);     // 无区间：T=1、S=0（恒等混合）
        return;
    }

    // ═══════════════════════════ §3 L1 tile 剪枝查找 ═══════════════════════════
    ivec2 texel_size = textureSize(uTileFogIndices, 0);
    int grid_cols = texel_size.x / TEXELS_PER_TILE;
    int grid_rows = texel_size.y;
    int tile_col = clamp(int(gl_FragCoord.x) / TILE_SIZE, 0, grid_cols - 1);
    int tile_row = clamp(int(gl_FragCoord.y) / TILE_SIZE, 0, grid_rows - 1);

    uint fog_idx[MAX_FOGS_PER_TILE];
    int nf = 0;
    for (int t = 0; t < TEXELS_PER_TILE; ++t) {
        vec4 px = texelFetch(uTileFogIndices,
                             ivec2(tile_col * TEXELS_PER_TILE + t, tile_row), 0);
        uint ch[4];
        ch[0] = uint(px.r * 255.0 + 0.5) & 255u;
        ch[1] = uint(px.g * 255.0 + 0.5) & 255u;
        ch[2] = uint(px.b * 255.0 + 0.5) & 255u;
        ch[3] = uint(px.a * 255.0 + 0.5) & 255u;
        for (int e = 0; e < 4; ++e) {
            if (ch[e] != FOG_INDEX_SENTINEL && ch[e] < uint(uTotalFogs)
                && nf < MAX_FOGS_PER_TILE) {
                fog_idx[nf] = ch[e];
                ++nf;
            }
        }
    }

    // ═══════════════════════════ §4 逐团采样累加（L2）═══════════════════════════
    float zs[ZC];
    float taus[ZC];
    vec3  eds[ZC];
    int m = 2;
    zs[0] = z_near;  zs[1] = z_far;
    taus[0] = 0.0;   taus[1] = 0.0;
    eds[0] = vec3(0.0); eds[1] = vec3(0.0);

    ivec2 pix = ivec2(gl_FragCoord.xy);
    for (int j = 0; j < nf; ++j) {
        int bi = int(fog_idx[j]);
        // 团属性：3 texel/团 ——（center.xyz, radius）（color.rgb, intensity）（attenuation,0,0）
        vec4 t0 = texelFetch(uFogBodyTex, ivec2(bi * 3 + 0, 0), 0);
        vec4 t1 = texelFetch(uFogBodyTex, ivec2(bi * 3 + 1, 0), 0);
        vec4 t2 = texelFetch(uFogBodyTex, ivec2(bi * 3 + 2, 0), 0);
        vec3  c   = t0.xyz;
        float rad = t0.w;
        vec3  col = t1.xyz;
        float inten = t1.w;
        int   atten = int(t2.x + 0.5);
        if (rad <= 0.0 || inten <= 0.0) {
            continue;
        }

        // 球求交（球弦 = 稳定域；深度裁剪只影响后续 ramp，不改采样点 ⇒ 不闪）。
        vec3 oc = ro - c;
        float bq = dot(oc, rd);
        float cc = dot(oc, oc) - rad * rad;
        float disc = bq * bq - cc;
        if (disc < 0.0) {
            continue;
        }
        float sq = sqrt(disc);
        float cs0 = max(-bq - sq, z_near);        // 弦的近端裁到近平面（不裁 scene_t）
        float cs1 = -bq + sq;                     // 弦的远端（球面出射）
        if (cs1 <= cs0) {
            continue;
        }
        float seg_len = (cs1 - cs0) / float(SEG_PER_FOG);

        for (int k = 0; k < SEG_PER_FOG; ++k) {
            float s0 = cs0 + float(k) * seg_len;
            float s1 = s0 + seg_len;
            // 段内抖动：在段内取一个采样点（确定性的屏幕坐标哈希）。
            float jit = Hash01(uvec3(uint(pix.x), uint(pix.y),
                                     uint(bi) * 131u + uint(k)));
            float ts = s0 + jit * seg_len;
            float rr = length(ro + ts * rd - c);
            float wgt = ProfileAtten(atten, rr / rad);
            if (wgt <= 0.0) {
                continue;
            }
            // MVP 光照：不采样 sun/shadow/ambient，L_in = 常量发射色；σ = intensity·w。
            ZAddSegment(zs, taus, eds, m, s0, s1, inten * wgt, col);
        }
    }

    // ═══════════════════════════ §5 ZDist 降维（≤8 控制点）═══════════════════════════
    int n = m;
    float fz[ZDIST_K];
    float ftau[ZDIST_K];
    vec3  fed[ZDIST_K];
    if (m <= ZDIST_K) {
        for (int i = 0; i < m; ++i) {
            fz[i] = zs[i]; ftau[i] = taus[i]; fed[i] = eds[i];
        }
    } else {
        // 前缀和（成本函数的系数）。四通道前缀和展平为一维（GLSL 3.30 不支持数组的数组）。
        float p0[ZC1];
        float pz[ZC1];
        float pzz[ZC1];
        float py[4 * ZC1];
        float pzy[4 * ZC1];
        float pyy[4 * ZC1];
        p0[0] = 0.0; pz[0] = 0.0; pzz[0] = 0.0;
        for (int c = 0; c < 4; ++c) {
            py[c * ZC1 + 0] = 0.0;
            pzy[c * ZC1 + 0] = 0.0;
            pyy[c * ZC1 + 0] = 0.0;
        }
        for (int i = 0; i + 1 < m; ++i) {
            float zi = zs[i];
            float zj = zs[i + 1];
            float w = exp(-0.5 * (taus[i] + taus[i + 1]));
            float g = (zj - zi) / 3.0 * w;
            p0[i + 1]  = p0[i]  + g;
            pz[i + 1]  = pz[i]  + g * (zi + zj);
            pzz[i + 1] = pzz[i] + g * (zi * zi + zi * zj + zj * zj);
            for (int c = 0; c < 4; ++c) {
                float yi = ChanVal(taus, eds, i, c);
                float yj = ChanVal(taus, eds, i + 1, c);
                py[c * ZC1 + i + 1]  = py[c * ZC1 + i]  + g * (yi + yj);
                pzy[c * ZC1 + i + 1] = pzy[c * ZC1 + i]
                    + g * (2.0 * zi * yi + zi * yj + zj * yi + 2.0 * zj * yj);
                pyy[c * ZC1 + i + 1] = pyy[c * ZC1 + i]
                    + g * (yi * yi + yi * yj + yj * yj);
            }
        }

        // 链表 + 存活标记 + 二叉最小堆（惰性失效）。
        int prev[ZC];
        int next_[ZC];
        bool alive[ZC];
        float h_inc[HEAP_CAP];
        int h_i[HEAP_CAP];
        int h_l[HEAP_CAP];
        int h_r[HEAP_CAP];
        int hn = 0;
        for (int i = 0; i < m; ++i) {
            prev[i] = i - 1;
            next_[i] = (i + 1 < m) ? (i + 1) : -1;
            alive[i] = true;
        }
        // push 内部点 i 的合并增量（端点不入堆）。
        // —— 内联 push（GLSL 无闭包/无引用捕获）。
        for (int i = 1; i + 1 < m; ++i) {
            int p = prev[i];
            int q = next_[i];
            float inc = ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, p, q)
                        - ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, p, i)
                        - ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, i, q);
            int idx = hn;
            ++hn;
            h_inc[idx] = inc; h_i[idx] = i; h_l[idx] = p; h_r[idx] = q;
            while (idx > 0) {
                int par = (idx - 1) / 2;
                if (h_inc[par] <= h_inc[idx]) {
                    break;
                }
                float ti = h_inc[par]; h_inc[par] = h_inc[idx]; h_inc[idx] = ti;
                int   tj = h_i[par];   h_i[par]   = h_i[idx];   h_i[idx]   = tj;
                tj = h_l[par]; h_l[par] = h_l[idx]; h_l[idx] = tj;
                tj = h_r[par]; h_r[par] = h_r[idx]; h_r[idx] = tj;
                idx = par;
            }
        }
        int count = m;
        while (count > ZDIST_K) {
            if (hn <= 0) {
                break;                     // 防御：不应发生（内部点恒在堆中）
            }
            // pop 堆顶（最小）。
            float it_inc = h_inc[0];
            int it_i = h_i[0];
            int it_l = h_l[0];
            int it_r = h_r[0];
            --hn;
            h_inc[0] = h_inc[hn]; h_i[0] = h_i[hn]; h_l[0] = h_l[hn]; h_r[0] = h_r[hn];
            int idx = 0;
            for (;;) {
                int l = 2 * idx + 1;
                int rc = 2 * idx + 2;
                int sm = idx;
                if (l < hn && h_inc[l] < h_inc[sm]) {
                    sm = l;
                }
                if (rc < hn && h_inc[rc] < h_inc[sm]) {
                    sm = rc;
                }
                if (sm == idx) {
                    break;
                }
                float tj = h_inc[sm]; h_inc[sm] = h_inc[idx]; h_inc[idx] = tj;
                int   tk = h_i[sm];   h_i[sm]   = h_i[idx];   h_i[idx]   = tk;
                tk = h_l[sm]; h_l[sm] = h_l[idx]; h_l[idx] = tk;
                tk = h_r[sm]; h_r[sm] = h_r[idx]; h_r[idx] = tk;
                idx = sm;
            }
            if (!alive[it_i] || prev[it_i] != it_l || next_[it_i] != it_r) {
                continue;                          // 陈旧条目
            }
            int p = prev[it_i];
            int q = next_[it_i];
            next_[p] = q;
            prev[q] = p;
            alive[it_i] = false;
            --count;
            // 邻居 p、q 现在成了新的内部点候选 → 重新入堆（内联 push，两次）。
            for (int pass = 0; pass < 2; ++pass) {
                int i = (pass == 0) ? p : q;
                if (i < 0 || i >= m || !alive[i]) {
                    continue;
                }
                int pp = prev[i];
                int qq = next_[i];
                if (pp < 0 || qq < 0) {
                    continue;
                }
                float inc = ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, pp, qq)
                            - ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, pp, i)
                            - ZCost(zs, taus, eds, p0, pz, pzz, py, pzy, pyy, i, qq);
                int hidx = hn;
                ++hn;
                h_inc[hidx] = inc; h_i[hidx] = i; h_l[hidx] = pp; h_r[hidx] = qq;
                while (hidx > 0) {
                    int par = (hidx - 1) / 2;
                    if (h_inc[par] <= h_inc[hidx]) {
                        break;
                    }
                    float ti = h_inc[par]; h_inc[par] = h_inc[hidx]; h_inc[hidx] = ti;
                    int   tj = h_i[par];   h_i[par]   = h_i[hidx];   h_i[hidx]   = tj;
                    tj = h_l[par]; h_l[par] = h_l[hidx]; h_l[hidx] = tj;
                    tj = h_r[par]; h_r[par] = h_r[hidx]; h_r[hidx] = tj;
                    hidx = par;
                }
            }
        }
        n = 0;
        for (int i = 0; i < m; ++i) {
            if (alive[i] && n < ZDIST_K) {
                fz[n] = zs[i]; ftau[n] = taus[i]; fed[n] = eds[i];
                ++n;
            }
        }
    }

    // ═══════════════════════════ §6 末端积分（Stieltjes 闭式）═══════════════════════════
    float T = exp(-ftau[n - 1]);
    vec3 S = vec3(0.0);
    for (int i = 0; i + 1 < n; ++i) {
        float dtau = ftau[i + 1] - ftau[i];
        float ratio;
        if (abs(dtau) < 1e-6) {
            ratio = 1.0 - 0.5 * dtau + (1.0 / 6.0) * dtau * dtau;   // (1−e^{−x})/x 的 x→0 极限
        } else {
            ratio = (1.0 - exp(-dtau)) / dtau;
        }
        S += (fed[i + 1] - fed[i]) * (exp(-ftau[i]) * ratio);
    }

    // ═══════════════════════════ §7 输出（就地混合）═══════════════════════════
    // glBlendFunc(GL_ONE, GL_SRC_ALPHA)：out.rgb = S + dst.rgb·T = L_scene·T + S。
    FragColor = vec4(S, T);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_

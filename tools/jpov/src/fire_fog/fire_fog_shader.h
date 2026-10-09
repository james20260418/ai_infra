// JPOV Fire-Fog — GLSL 着色器（低分辨率 ZDist + 屏幕空间深度敏感高斯）
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §3（ZDist）与 §14（屏幕空间高斯）。
//
// 管线（Danis 2026-10-09 定「简单加权」版）分三趟，ZDist 全部在**低分辨率**域：
//   A 累加（fire_fog_zdist）  低分辨率：tile 剪枝 → 逐团 M 段采样 → ZDist 累加 → 降维 ≤8
//                             → 打包成 6 个 RGBA32F texel（见 zdist_texture_layout.h）。
//   B 合并（fire_fog_gauss）  低分辨率：读中心 + 核内邻居的 ZDist → **简单加权**合并
//                             （权重 g_i·ρ_i，非重叠段用中心 Z0 补全）→ 降维 ≤8 → 打包。
//   C 合成（fire_fog_compose）主分辨率：逐像素对合并后的 ZDist 做**双线性**重建 → 末端积分
//                             → 就地混合到当前 3D HDR FBO（GL_ONE / GL_SRC_ALPHA）。
//
// 「简单加权」的合并公式（Danis 2026-10-09 确认；标量分母版）：
//   中心像素 0，Z0 定义域 [z_near, b0]；邻居 i：Zi 定义域 [z_near, b_i]（所有像素共用一个
//   z_near）。令 overlap 占比 ρ_i = (min(b0,b_i) − z_near)/(b0 − z_near)，高斯权重
//   g_i = exp(−|Δ_i|²/(2σ²))，w_i = g_i·ρ_i（ρ_i = 0 ⇒ 该邻居被真正跳过）。
//     Ẑ_i(z) = Zi(z) if z ≤ min(b0,b_i) else Z0(z)          // 非重叠段用 Z0 补全
//     N(z)   = w0·Z0(z) + Σ_i w_i·Ẑ_i(z)   （w0 = 1）
//     D      = w0 + Σ_i w_i                （标量）
//     F(z)   = N(z) / D,  z ∈ [z_near, b0]
//   N 建在「Z0 断点 ∪ 各邻居断点 ∪ 各 overlap 端点」的共享 z 网格上（放得下就放），
//   再对 F 做二叉最小堆贪心降维到 ≤8（与 CPU GreedyReduceFast 一致，O(m log m)）。
//
// ⚠️ shader 里的 #define 常量必须与 fire_fog_renderer.h / zdist_texture_layout.h 对应。

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

// 三趟共用的 GLSL 前置（函数库）。各趟 FS = "#version 330 core\n" + 本串 + <趟体>。
inline constexpr const char* kFireFogCommonGlsl = R"glsl(

// ── tile culling 常量（与 fire_fog_renderer.h 对应）──
#define TILE_SIZE           16
#define MAX_FOGS_PER_TILE    8
#define TEXELS_PER_TILE      2
#define FOG_INDEX_SENTINEL   255u

// ── ZDist 常量（与 zdist_function.h 对应）──
#define ZDIST_K              8     // 控制点上限
#define SEG_PER_FOG          2     // M：每团 z 段数
#define ZC                  40     // 候选断点 / 合并网格上限
#define ZC1                 41
#define HEAP_CAP           128     // 降维堆容量
#define TAU_MAX           32.0     // τd 的 uint16 归一化量程（与 zdist_texture_layout.h 同）

// ── 高斯核：邻居偏移半径上限（编译期常量；运行期用 uKernelRadius 夹紧）──
#define MAX_KERNEL_RADIUS    3

in vec2 vTexCoord;

// ═══════════════════════════ 通用工具 ═══════════════════════════

uint HashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float Hash01(uvec3 s) {
    uint h = HashU(s.x + s.y * 0x9e3779b9u + s.z * 0x85ebca6bu);
    return float(h & 0x00FFFFFFu) / float(0x01000000u);
}
float ProfileAtten(int kind, float u) {
    if (u >= 1.0) {
        return 0.0;
    }
    u = clamp(u, 0.0, 1.0);
    if (kind == 0) {
        return 1.0;
    }
    if (kind == 1) {
        return 1.0 - u;
    }
    if (kind == 2) {
        return 1.0 - u * u;
    }
    const float k = 4.0;
    return (exp(-k * u) - exp(-k)) / (1.0 - exp(-k));
}

// ── 位/精度打包（与 zdist_texture_layout.h 的 zdist_pack_detail 逐式一致）──
float U2F(uint u) { return uintBitsToFloat(u); }
uint  F2U(float f) { return floatBitsToUint(f); }

uint F2H(float value) {                       // float → IEEE binary16 位模式（RNE）
    uint x = floatBitsToUint(value);
    uint sign = (x >> 16) & 0x8000u;
    int exponent = int((x >> 23) & 0xffu) - 127 + 15;
    uint mantissa = x & 0x007fffffu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return sign;
        }
        uint m = mantissa | 0x00800000u;
        uint shift = uint(14 - exponent);
        uint rounded = (m + (1u << (shift - 1u)) - 1u + ((m >> shift) & 1u)) >> shift;
        return sign | rounded;
    }
    if (exponent >= 31) {
        if (((x >> 23) & 0xffu) == 0xffu && mantissa != 0u) {
            return sign | 0x7c00u | 0x0200u;
        }
        return sign | 0x7c00u;
    }
    uint rounded = (mantissa + 0x00000fffu + ((mantissa >> 13) & 1u)) >> 13;
    return sign | (uint(exponent) << 10) | rounded;
}
float H2F(uint h) {                           // IEEE binary16 位模式 → float
    uint sign = (h & 0x8000u) << 16;
    uint exp = (h >> 10) & 0x1fu;
    uint man = h & 0x3ffu;
    uint outb;
    if (exp == 0u) {
        if (man == 0u) {
            outb = sign;
        } else {
            int e = -1;
            while ((man & 0x400u) == 0u) {
                man <<= 1;
                ++e;
            }
            man &= 0x3ffu;
            outb = sign | (uint(127 - 15 - e) << 23) | (man << 13);
        }
    } else if (exp == 31u) {
        outb = sign | 0x7f800000u | (man << 13);
    } else {
        outb = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    return uintBitsToFloat(outb);
}
uint EncTau(float tau) {
    float q = clamp(tau / TAU_MAX * 65535.0, 0.0, 65535.0);
    return uint(q + 0.5);
}
float DecTau(uint q) {
    return float(q) / 65535.0 * TAU_MAX;
}

// ── ZDist 分段线性求值（域外按端点夹断）──
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
        return;
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

// 叠加一个 z 段 [z0,z1]（消光 sigma、内散射色 Lin）。超出域的部分裁掉。
void ZAddSegment(inout float zs[ZC], inout float taus[ZC], inout vec3 eds[ZC],
                 inout int m, float z0, float z1, float sigma, vec3 Lin) {
    float c0 = max(z0, zs[0]);
    float c1 = min(z1, zs[m - 1]);
    if (c1 <= c0) {
        return;
    }
    float len = c1 - c0;
    float rise_tau = sigma * len;
    vec3  rise_ed  = Lin * (sigma * len);
    ZInsertBreakpoint(zs, taus, eds, m, c0);
    ZInsertBreakpoint(zs, taus, eds, m, c1);
    for (int i = 0; i < m; ++i) {
        float f = clamp((zs[i] - c0) / (c1 - c0), 0.0, 1.0);
        taus[i] += f * rise_tau;
        eds[i]  += rise_ed * f;
    }
}

// ═══════════════════════════ ZDist 降维（二叉最小堆贪心，O(m log m)）═══════════════════════════
// 与 CPU zdist_function.h 的 GreedyReduceFast / 前缀代价 PrefixCost 逐式一致。

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

// 把 (zs,taus,eds) 的 m 个点贪心降到 ≤ZDIST_K。m ≤ 8 时原样拷贝。
void Reduce8(inout float zs[ZC], inout float taus[ZC], inout vec3 eds[ZC], inout int m,
             out float fz[ZC], out float ftau[ZC], out vec3 fed[ZC],
             out int fn) {
    if (m <= ZDIST_K) {
        for (int i = 0; i < m; ++i) {
            fz[i] = zs[i];
            ftau[i] = taus[i];
            fed[i] = eds[i];
        }
        fn = m;
        return;
    }
    float p0[ZC1];
    float pz[ZC1];
    float pzz[ZC1];
    float py[4 * ZC1];
    float pzy[4 * ZC1];
    float pyy[4 * ZC1];
    p0[0] = 0.0;
    pz[0] = 0.0;
    pzz[0] = 0.0;
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
            break;
        }
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
            continue;
        }
        int p = prev[it_i];
        int q = next_[it_i];
        next_[p] = q;
        prev[q] = p;
        alive[it_i] = false;
        --count;
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
    fn = 0;
    for (int i = 0; i < m; ++i) {
        if (alive[i] && fn < ZDIST_K) {
            fz[fn] = zs[i];
            ftau[fn] = taus[i];
            fed[fn] = eds[i];
            ++fn;
        }
    }
}

// ═══════════════════════════ 末端积分（Stieltjes 闭式）═══════════════════════════
void EndIntegrate(float zs[ZC], float taus[ZC], vec3 eds[ZC], int n,
                  out vec3 S, out float T) {
    T = exp(-taus[n - 1]);
    S = vec3(0.0);
    for (int i = 0; i + 1 < n; ++i) {
        float dtau = taus[i + 1] - taus[i];
        float ratio;
        if (abs(dtau) < 1e-6) {
            ratio = 1.0 - 0.5 * dtau + (1.0 / 6.0) * dtau * dtau;
        } else {
            ratio = (1.0 - exp(-dtau)) / dtau;
        }
        S += (eds[i + 1] - eds[i]) * (exp(-taus[i]) * ratio);
    }
}

// ═══════════════════════════ 6-texel 打包 / 解包（与 zdist_texture_layout.h 同布局）═══════════════════════════
// lane[0..8) z fp32；lane[8..12) τd uint16（2/lane）；lane[12..24) Ed fp16（2/lane）。
void PackZDist(float zs[ZC], float taus[ZC], vec3 eds[ZC], int n,
               out uint L[24]) {
    for (int i = 0; i < 24; ++i) {
        L[i] = 0u;
    }
    for (int i = 0; i < ZDIST_K; ++i) {
        int s = (i < n) ? i : n - 1;
        L[i] = F2U(zs[s]);
        uint qt = EncTau(taus[s]);
        int li = 8 + i / 2;
        if ((i & 1) == 0) {
            L[li] = (L[li] & 0xffff0000u) | qt;
        } else {
            L[li] = (L[li] & 0x0000ffffu) | (qt << 16);
        }
        for (int c = 0; c < 3; ++c) {
            int j = i * 3 + c;
            uint qe = F2H(eds[s][c]);
            int lj = 12 + j / 2;
            if ((j & 1) == 0) {
                L[lj] = (L[lj] & 0xffff0000u) | qe;
            } else {
                L[lj] = (L[lj] & 0x0000ffffu) | (qe << 16);
            }
        }
    }
}

void UnpackZDist(uint L[24], out float zs[ZC], out float taus[ZC],
                 out vec3 eds[ZC], out int n) {
    n = 0;
    for (int i = 0; i < ZDIST_K; ++i) {
        float z = U2F(L[i]);
        if (i > 0 && z <= zs[n - 1]) {
            break;
        }
        zs[n] = z;
        uint qt = ((i & 1) == 0) ? (L[8 + i / 2] & 0xffffu) : (L[8 + i / 2] >> 16);
        taus[n] = DecTau(qt);
        vec3 e;
        for (int c = 0; c < 3; ++c) {
            int j = i * 3 + c;
            uint qe = ((j & 1) == 0) ? (L[12 + j / 2] & 0xffffu) : (L[12 + j / 2] >> 16);
            e[c] = H2F(qe);
        }
        eds[n] = e;
        ++n;
    }
    // 防御：至少 2 点（正常情况下打包必然 ≥2）。
    if (n < 1) {
        zs[0] = 0.0;
        taus[0] = 0.0;
        eds[0] = vec3(0.0);
        n = 1;
    }
    if (n < 2) {
        zs[1] = zs[0] + 1e-3;
        taus[1] = taus[0];
        eds[1] = eds[0];
        n = 2;
    }
    // 把 [n, ZDIST_K) 的空槽填成末点（与 PackZDist 的补位约定一致）——
    // 趟 C 的双线性对全部 8 槽插值，读未初始化槽会污染结果。
    for (int i = n; i < ZDIST_K; ++i) {
        zs[i] = zs[n - 1];
        taus[i] = taus[n - 1];
        eds[i] = eds[n - 1];
    }
}

// 读 6 个 RGBA32F texel（已展开成 vec4）里的 24 lane。
void LoadLanes(vec4 a0, vec4 a1, vec4 a2, vec4 a3, vec4 a4, vec4 a5, out uint L[24]) {
    L[0] = F2U(a0.x);  L[1] = F2U(a0.y);  L[2] = F2U(a0.z);  L[3] = F2U(a0.w);
    L[4] = F2U(a1.x);  L[5] = F2U(a1.y);  L[6] = F2U(a1.z);  L[7] = F2U(a1.w);
    L[8] = F2U(a2.x);  L[9] = F2U(a2.y);  L[10] = F2U(a2.z); L[11] = F2U(a2.w);
    L[12] = F2U(a3.x); L[13] = F2U(a3.y); L[14] = F2U(a3.z); L[15] = F2U(a3.w);
    L[16] = F2U(a4.x); L[17] = F2U(a4.y); L[18] = F2U(a4.z); L[19] = F2U(a4.w);
    L[20] = F2U(a5.x); L[21] = F2U(a5.y); L[22] = F2U(a5.z); L[23] = F2U(a5.w);
}

// 读 ZDist 纹理组（6 张）在像素 p 的值 → 控制点。
void FetchZDist(sampler2D s0, sampler2D s1, sampler2D s2,
                sampler2D s3, sampler2D s4, sampler2D s5,
                ivec2 p, out float zs[ZC], out float taus[ZC],
                out vec3 eds[ZC], out int n) {
    uint L[24];
    LoadLanes(texelFetch(s0, p, 0), texelFetch(s1, p, 0), texelFetch(s2, p, 0),
              texelFetch(s3, p, 0), texelFetch(s4, p, 0), texelFetch(s5, p, 0), L);
    UnpackZDist(L, zs, taus, eds, n);
}
)glsl";

// ── 趟 A：低分辨率 ZDist 累加（tile 剪枝 + 逐团采样 + 降维 + 打包成 6 输出）──
inline constexpr const char* kFireFogZdstBody = R"glsl(

uniform sampler2D uSceneDepthTex;
uniform sampler2D uTileFogIndices;
uniform sampler2D uFogBodyTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;
uniform float uZFar;
uniform int   uTotalFogs;
uniform int   uJitterEnable;     // 0 = 关抖动（用段中点）

out vec4 out0;
out vec4 out1;
out vec4 out2;
out vec4 out3;
out vec4 out4;
out vec4 out5;

void main() {
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 ro = uCamPos;
    vec3 rd = normalize(pf.xyz / pf.w - ro);

    float dndc = texture(uSceneDepthTex, vTexCoord).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    float scene_t = length(pw.xyz / pw.w - ro);

    float z_near = uZNear;
    float z_far = min(uZFar, scene_t);
    if (z_far <= z_near + 1e-6) {
        out0 = vec4(0.0); out1 = vec4(0.0); out2 = vec4(0.0);
        out3 = vec4(0.0); out4 = vec4(0.0); out5 = vec4(0.0);
        return;
    }

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
        vec3 oc = ro - c;
        float bq = dot(oc, rd);
        float cc = dot(oc, oc) - rad * rad;
        float disc = bq * bq - cc;
        if (disc < 0.0) {
            continue;
        }
        float sq = sqrt(disc);
        float cs0 = max(-bq - sq, z_near);
        float cs1 = -bq + sq;
        if (cs1 <= cs0) {
            continue;
        }
        float seg_len = (cs1 - cs0) / float(SEG_PER_FOG);
        for (int k = 0; k < SEG_PER_FOG; ++k) {
            float s0 = cs0 + float(k) * seg_len;
            float s1 = s0 + seg_len;
            float jit = (uJitterEnable != 0)
                ? Hash01(uvec3(uint(pix.x), uint(pix.y), uint(bi) * 131u + uint(k)))
                : 0.5;
            float ts = s0 + jit * seg_len;
            float rr = length(ro + ts * rd - c);
            float wgt = ProfileAtten(atten, rr / rad);
            if (wgt <= 0.0) {
                continue;
            }
            ZAddSegment(zs, taus, eds, m, s0, s1, inten * wgt, col);
        }
    }

    float fz[ZC];
    float ftau[ZC];
    vec3  fed[ZC];
    int fn;
    Reduce8(zs, taus, eds, m, fz, ftau, fed, fn);

    uint L[24];
    PackZDist(fz, ftau, fed, fn, L);
    out0 = vec4(U2F(L[0]), U2F(L[1]), U2F(L[2]), U2F(L[3]));
    out1 = vec4(U2F(L[4]), U2F(L[5]), U2F(L[6]), U2F(L[7]));
    out2 = vec4(U2F(L[8]), U2F(L[9]), U2F(L[10]), U2F(L[11]));
    out3 = vec4(U2F(L[12]), U2F(L[13]), U2F(L[14]), U2F(L[15]));
    out4 = vec4(U2F(L[16]), U2F(L[17]), U2F(L[18]), U2F(L[19]));
    out5 = vec4(U2F(L[20]), U2F(L[21]), U2F(L[22]), U2F(L[23]));
}
)glsl";

// ── 趟 B：低分辨率屏幕空间深度敏感高斯合并（简单加权：g·ρ + Z0 补全）──
inline constexpr const char* kFireFogGaussBody = R"glsl(

uniform sampler2D uZDist0;
uniform sampler2D uZDist1;
uniform sampler2D uZDist2;
uniform sampler2D uZDist3;
uniform sampler2D uZDist4;
uniform sampler2D uZDist5;
uniform int   uKernelRadius;     // 邻居偏移半径（低分辨率像素域）
uniform float uGaussSigma;       // 高斯 σ（低分辨率像素域）

out vec4 out0;
out vec4 out1;
out vec4 out2;
out vec4 out3;
out vec4 out4;
out vec4 out5;

void main() {
    ivec2 size = textureSize(uZDist0, 0);
    ivec2 cp = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - ivec2(1));

    float zs0[ZC];
    float tau0[ZC];
    vec3  ed0[ZC];
    int n0;
    FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5,
               cp, zs0, tau0, ed0, n0);
    float z_near = zs0[0];
    float b0 = zs0[n0 - 1];
    float span0 = max(b0 - z_near, 1e-6);

    // 合并网格（N）：初始 = w0·Z0（w0 = 1），并含 Z0 的全部断点。
    float gz[ZC];
    float gNt[ZC];
    vec3  gNe[ZC];
    int gm = n0;
    for (int i = 0; i < n0; ++i) {
        gz[i] = zs0[i];
        gNt[i] = tau0[i];
        gNe[i] = ed0[i];
    }
    float D = 1.0;                                       // 分母（标量）

    float inv2sig2 = 1.0 / (2.0 * max(uGaussSigma, 1e-3) * max(uGaussSigma, 1e-3));

    for (int dy = -MAX_KERNEL_RADIUS; dy <= MAX_KERNEL_RADIUS; ++dy) {
        for (int dx = -MAX_KERNEL_RADIUS; dx <= MAX_KERNEL_RADIUS; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (abs(dx) > uKernelRadius || abs(dy) > uKernelRadius) {
                continue;
            }
            ivec2 np = clamp(cp + ivec2(dx, dy), ivec2(0), size - ivec2(1));
            float zsi[ZC];
            float taui[ZC];
            vec3  edi[ZC];
            int ni;
            FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5,
                       np, zsi, taui, edi, ni);
            float bi = zsi[ni - 1];
            float oe = min(b0, bi);                        // overlap 端点
            float rho = (oe - z_near) / span0;             // z 重叠占比
            if (rho <= 0.0) {
                continue;                                  // 无 z 重叠 → 真正跳过
            }
            float gauss = exp(-float(dx * dx + dy * dy) * inv2sig2);
            float wgt = gauss * clamp(rho, 0.0, 1.0);

            // 先把邻居的断点（≤ oe 的部分）与 overlap 端点并入网格（保持 N 连续）。
            for (int k = 0; k < ni; ++k) {
                if (zsi[k] > z_near && zsi[k] <= oe && gm < ZC) {
                    ZInsertBreakpoint(gz, gNt, gNe, gm, zsi[k]);
                }
            }
            if (oe > z_near && oe < b0 && gm < ZC) {
                ZInsertBreakpoint(gz, gNt, gNe, gm, oe);
            }

            // 再加 wgt·Ẑ_i：Ẑ_i = Zi on [z_near,oe]，Z0 otherwise。
            for (int j = 0; j < gm; ++j) {
                float z = gz[j];
                float vt;
                vec3 ve;
                if (z <= oe) {
                    vt = ZInterpTau(zsi, taui, ni, z);
                    ve = ZInterpEd(zsi, edi, ni, z);
                } else {
                    vt = ZInterpTau(zs0, tau0, n0, z);
                    ve = ZInterpEd(zs0, ed0, n0, z);
                }
                gNt[j] += wgt * vt;
                gNe[j] += wgt * ve;
            }
            D += wgt;
        }
    }

    float invD = 1.0 / D;
    for (int j = 0; j < gm; ++j) {
        gNt[j] *= invD;
        gNe[j] *= invD;
    }

    float fz[ZC];
    float ftau[ZC];
    vec3  fed[ZC];
    int fn;
    Reduce8(gz, gNt, gNe, gm, fz, ftau, fed, fn);

    uint L[24];
    PackZDist(fz, ftau, fed, fn, L);
    out0 = vec4(U2F(L[0]), U2F(L[1]), U2F(L[2]), U2F(L[3]));
    out1 = vec4(U2F(L[4]), U2F(L[5]), U2F(L[6]), U2F(L[7]));
    out2 = vec4(U2F(L[8]), U2F(L[9]), U2F(L[10]), U2F(L[11]));
    out3 = vec4(U2F(L[12]), U2F(L[13]), U2F(L[14]), U2F(L[15]));
    out4 = vec4(U2F(L[16]), U2F(L[17]), U2F(L[18]), U2F(L[19]));
    out5 = vec4(U2F(L[20]), U2F(L[21]), U2F(L[22]), U2F(L[23]));
}
)glsl";

// ── 趟 C：主分辨率合成（双线性重建 ZDist → 末端积分 → 就地混合）──
inline constexpr const char* kFireFogComposeBody = R"glsl(

uniform sampler2D uZDist0;
uniform sampler2D uZDist1;
uniform sampler2D uZDist2;
uniform sampler2D uZDist3;
uniform sampler2D uZDist4;
uniform sampler2D uZDist5;
uniform sampler2D uSceneDepthTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;
uniform float uZFar;

out vec4 FragColor;

void main() {
    // 主分辨率像素 → 低分辨率坐标（texel 中心对齐）。降采样倍数 N = 主尺寸 / 低分辨率尺寸。
    ivec2 lo = textureSize(uZDist0, 0);
    ivec2 depth_size = textureSize(uSceneDepthTex, 0);
    float Nx = float(depth_size.x) / max(float(lo.x), 1.0);
    float Ny = float(depth_size.y) / max(float(lo.y), 1.0);
    vec2 f = vec2(gl_FragCoord.x / Nx, gl_FragCoord.y / Ny) - vec2(0.5);

    ivec2 i0 = ivec2(floor(f));
    vec2 fr = f - vec2(i0);
    ivec2 sz = lo;
    ivec2 i00 = clamp(i0, ivec2(0), sz - ivec2(1));
    ivec2 i10 = clamp(i0 + ivec2(1, 0), ivec2(0), sz - ivec2(1));
    ivec2 i01 = clamp(i0 + ivec2(0, 1), ivec2(0), sz - ivec2(1));
    ivec2 i11 = clamp(i0 + ivec2(1, 1), ivec2(0), sz - ivec2(1));

    float zA[ZC]; float tA[ZC]; vec3 eA[ZC]; int nA;
    float zB[ZC]; float tB[ZC]; vec3 eB[ZC]; int nB;
    float zC[ZC]; float tC[ZC]; vec3 eC[ZC]; int nC;
    float zD[ZC]; float tD[ZC]; vec3 eD[ZC]; int nD;
    FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5, i00, zA, tA, eA, nA);
    FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5, i10, zB, tB, eB, nB);
    FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5, i01, zC, tC, eC, nC);
    FetchZDist(uZDist0, uZDist1, uZDist2, uZDist3, uZDist4, uZDist5, i11, zD, tD, eD, nD);

    // 逐槽双线性（8 槽定长；打包已把不足点补成重复末点）。
    float w00 = (1.0 - fr.x) * (1.0 - fr.y);
    float w10 = fr.x * (1.0 - fr.y);
    float w01 = (1.0 - fr.x) * fr.y;
    float w11 = fr.x * fr.y;

    float zs[ZC];
    float taus[ZC];
    vec3  eds[ZC];
    for (int i = 0; i < ZDIST_K; ++i) {
        zs[i]   = w00 * zA[i] + w10 * zB[i] + w01 * zC[i] + w11 * zD[i];
        taus[i] = w00 * tA[i] + w10 * tB[i] + w01 * tC[i] + w11 * tD[i];
        eds[i]  = w00 * eA[i] + w10 * eB[i] + w01 * eC[i] + w11 * eD[i];
    }

    // 场景深度裁剪：把 z 夹到 [z_near, scene_t]，并保证严格递增。
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    float dndc = texture(uSceneDepthTex, vTexCoord).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    vec3 ro = uCamPos;
    float scene_t = length(pw.xyz / pw.w - ro);

    // 去掉退化槽位 + 单调化。
    float mz[ZC];
    float mt[ZC];
    vec3  me[ZC];
    int mm = 0;
    for (int i = 0; i < ZDIST_K; ++i) {
        float z = clamp(zs[i], uZNear, max(scene_t, uZNear + 1e-4));
        if (mm > 0 && z <= mz[mm - 1] + 1e-6) {
            continue;                                     // 退化/夹断后重复 → 丢弃
        }
        mz[mm] = z;
        mt[mm] = taus[i];
        me[mm] = eds[i];
        ++mm;
    }
    if (mm < 2) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 S;
    float T;
    EndIntegrate(mz, mt, me, mm, S, T);
    FragColor = vec4(S, T);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_

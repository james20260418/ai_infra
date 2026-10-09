// JPOV Fire-Fog — GLSL 着色器（froxel 屏幕空间摊销 Z 轴采样）
//
// 设计见 tools/jpov/docs/jpov_froxel_design.md。趟（前两趟跑在 froxel 纹理上，最后一趟全屏）：
//   inject  （fire_fog_inject）    每个 texel = 某 froxel 的**局部** (τ, S)：按 tile 中心
//                                  视线，在 [z_k, z_{k+1}) 段内累加候选雾团的 Δτ 与 S_leaf。
//   reduce  （fire_fog_reduce）    金字塔每一级：由上一级沿 z **4 合 1**（over 合成），
//                                  得到「每块 = 4^j 个切片」的粗粒度块，供 scatter「大块跳」。
//   scatter （fire_fog_scatter）   每个 texel = 某 froxel 的**累积** (τ, S)：沿 z 做有序前缀，
//                                  用金字塔 + **base-4 分解** ⇒ 每 texel **O(log Nz)** 次合成。
//   composite（fire_fog_composite） 每个像素读 4 邻 tile 的 scatter 列、按像素深度 z 做
//                                  切片间插值 + 双线性，输出 vec4(S.rgb, T) 就地混合。
//
// froxel 网格：每个 Nxy 单元 = froxel 纹理上一块 sblock×sblock texel（sblock=√nz），
// 承载 nz 个 z 切片（指数分布，近密远疏）；texel 索引 k = iy*uBlockSide + ix（行主序）。
// 分辨率由运行期 uniform uNz / uBlockSide / uTilePx 给出（见 FireFogParams.nz / tile_px）。
//
// 存储语义（inject / 各级 / scatter 均为 RGBA32F）：RGBA = (τ, S.r, S.g, S.b)。
//   - inject 存该切片的**局部** (Δτ, S_leaf)，S_leaf = L_in·(1 − exp(−Δτ))。
//   - reduce 级 j 每个 cell = 上一级 4 个 cell（z 序）的 over 合成，覆盖 4^j 个切片。
//   - scatter 的 texel k 存**累积**「覆盖 [z_near, z_{k+1})」的 (τ, S)，其自然位置 = z_{k+1}。
// ⚠️ shader 里的 #define（tile 索引布局常量）必须与 fire_fog_renderer.h 对应。

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

// 各趟共用的 GLSL 前置（函数库）。各趟 FS = "#version 330 core\n" + 本串 + <趟体>。
inline constexpr const char* kFireFogCommonGlsl = R"glsl(

// ── tile 索引纹理布局常量（编译期，与 fire_fog_renderer.h 对应）──
#define MAX_FOGS_PER_TILE    8
#define TEXELS_PER_TILE      2
#define FOG_INDEX_SENTINEL   255u

// ── froxel 网格（运行期 uniform；见 FireFogParams.nz / tile_px）──
//   uNz        每柱 z 切片数（= uBlockSide²）
//   uBlockSide 每柱在 froxel 纹理上的 texel 边长（= √uNz）
//   uTilePx    每个 Nxy 单元对应的**屏幕**像素边长（每轴）
uniform int uNz;
uniform int uBlockSide;
uniform int uTilePx;

in vec2 vTexCoord;

// 径向衰减剖面权重（与 fog_body.h 的 FogAttenuation 枚举一致）。
// 返回 u∈[0,1] 处的相对消光权重；u >= 1 ⇒ 0（球外不贡献）。
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

// over 合成算子（z 序：A 在**近**、B 在**远**）。vec4 = (τ, S.rgb)。
//   τ = τA + τB；S = SA + exp(−τA)·SB
// 可结合、**不可交换**（合并顺序必须保持 z 序）。空段 = (0,0)（恒等）。
vec4 OverCompose(vec4 A, vec4 B) {
    return vec4(A.x + B.x, A.yzw + exp(-A.x) * B.yzw);
}
)glsl";

// ── 趟 1：inject —— 逐 froxel 局部 (τ, S) ──
inline constexpr const char* kFireFogInjectBody = R"glsl(

uniform sampler2D uTileFogIndices;
uniform sampler2D uTileZRange;   // RG32F：每 tile 的保守 z 范围 (zmin, zmax)
uniform sampler2D uFogBodyTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;          // froxel z 分布近端（米）
uniform float uR;              // z 生长系数 = (uZFar/uZNear)^(1/uNz)（CPU 预算，见 renderer）
uniform vec2  uFboSize;        // 主 FBO 像素尺寸（重建 tile 中心视线用）
uniform int   uTotalFogs;

// ── 光照（step2；step1 时 uSunEnable==0，L_in = col）──
uniform int   uSunEnable;
uniform vec3  uAmbientColor;
uniform float uAmbientIntensity;
uniform int   uHasSun;
uniform vec3  uSunColor;
uniform float uSunIntensity;
uniform vec3  uSunDir;         // 光传播方向（从太阳指向场景）
uniform float uSunPhaseG;
uniform float uSunGain;

// ── CSM 阴影（与 object3d 同一套资源；**原始单次采样，无 PCF**）──
uniform int   uCascadeCount;
uniform float uCascadeRanges[5];
uniform sampler2D uShadowMap[5];
uniform mat4  uShadowVP[5];
uniform mat4  uShadowDepthVP[5];
uniform float uShadowTexelWorld[5];
uniform float uShadowBiasCascade[5];
uniform int   uShadowBiasOverride;
uniform float uShadowFadeStart;
uniform float uShadowFadeEnd;

out vec4 oColor;   // (τ, S.rgb)

// 单级联单次采样（无 PCF）。因 GLSL 330 禁动态 sampler 索引，调用处按常量索引展开。
float CsmTap(sampler2D smap, mat4 vp, mat4 dvp, float texelW,
             float biasVal, int biasOv, vec3 wp) {
    vec4 lsp = vp * vec4(wp, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;                        // 不在该级联覆盖内 ⇒ 视为受照
    }
    vec4 dpos = dvp * vec4(wp, 1.0);
    float cur = dpos.z / dpos.w;
    float tw = (biasOv != 0) ? biasVal : texelW;
    cur -= max(0.01, 1.5 * tw);            // 雾无表面法线 ⇒ 用 tanθ=1 的保守深度偏置
    return (cur <= texture(smap, uv).r) ? 1.0 : 0.0;
}

// Henyey-Greenstein 散射相位（相对太阳→视线的散射角）。μ=1 = 朝太阳看（前向散射）。
float SunPhase(vec3 rd) {
    vec3 sd = normalize(uSunDir);
    float mu = clamp(dot(normalize(rd), -sd), -1.0, 1.0);
    float g = clamp(uSunPhaseG, 0.0, 0.95);
    float denom = 1.0 + g * g - 2.0 * g * mu;
    return uSunGain * (1.0 - g * g)
           / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

// 原始 CSM（单次采样，无 PCF）：按采样点到相机的距离选主级联，末尾按 fade 淡出。
float CsmShadow(vec3 wp, float dist) {
    if (uHasSun == 0 || uCascadeCount <= 0) {
        return 1.0;
    }
    int c = uCascadeCount - 1;
    for (int i = 0; i < 5; ++i) {
        if (i < uCascadeCount && dist <= uCascadeRanges[i]) {
            c = i;
            break;
        }
    }
    float s;
    if (c == 0) {
        s = CsmTap(uShadowMap[0], uShadowVP[0], uShadowDepthVP[0], uShadowTexelWorld[0],
                   uShadowBiasCascade[0], uShadowBiasOverride, wp);
    } else if (c == 1) {
        s = CsmTap(uShadowMap[1], uShadowVP[1], uShadowDepthVP[1], uShadowTexelWorld[1],
                   uShadowBiasCascade[1], uShadowBiasOverride, wp);
    } else if (c == 2) {
        s = CsmTap(uShadowMap[2], uShadowVP[2], uShadowDepthVP[2], uShadowTexelWorld[2],
                   uShadowBiasCascade[2], uShadowBiasOverride, wp);
    } else if (c == 3) {
        s = CsmTap(uShadowMap[3], uShadowVP[3], uShadowDepthVP[3], uShadowTexelWorld[3],
                   uShadowBiasCascade[3], uShadowBiasOverride, wp);
    } else {
        s = CsmTap(uShadowMap[4], uShadowVP[4], uShadowDepthVP[4], uShadowTexelWorld[4],
                   uShadowBiasCascade[4], uShadowBiasOverride, wp);
    }
    float fade = 1.0 - clamp((dist - uShadowFadeStart)
                             / max(uShadowFadeEnd - uShadowFadeStart, 1e-5), 0.0, 1.0);
    return mix(1.0, s, fade);
}

void main() {
    ivec2 frag = ivec2(gl_FragCoord.xy);
    ivec2 tex_sz = textureSize(uTileFogIndices, 0);
    int grid_cols = tex_sz.x / TEXELS_PER_TILE;
    int grid_rows = tex_sz.y;
    int tile_col = clamp(frag.x / uBlockSide, 0, grid_cols - 1);
    int tile_row = clamp(frag.y / uBlockSide, 0, grid_rows - 1);
    int ix = frag.x - tile_col * uBlockSide;
    int iy = frag.y - tile_row * uBlockSide;
    int k = iy * uBlockSide + ix;

    // 本 froxel 的 z 区间 [z_k, z_{k+1})（指数分布）。
    float zk  = uZNear * pow(uR, float(k));
    float zk1 = zk * uR;

    // ★ 短路：本 tile 候选团的保守 z 范围是 [zr.x, zr.y]；切片若完全在其外 ⇒ 局部恒等
    //（省掉下方 ≤8 个团的射线-球求交 + CSM 采样）。空 tile（无候选）zr=(0,0) ⇒ 恒等。
    vec2 zr = texelFetch(uTileZRange, ivec2(tile_col, tile_row), 0).xy;
    if (zk1 <= zr.x || zk >= zr.y) {
        oColor = vec4(0.0);
        return;
    }

    // tile 中心视线（froxel = 整柱一个值，用 tile 中心而非像素自身视线）。
    vec2 center_px = vec2(float(tile_col * uBlockSide) + 0.5 * float(uBlockSide),
                          float(tile_row * uBlockSide) + 0.5 * float(uBlockSide));
    vec2 uv = center_px / uFboSize;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 ro = uCamPos;
    vec3 rd = normalize(pf.xyz / pf.w - ro);

    // 本 tile 的候选雾团索引。
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

    vec4 acc = vec4(0.0);   // 局部 (τ, S)，本 froxel 内多团 over 合成（顺序无关）
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
        // 球 × 光线求交。
        vec3 oc = ro - c;
        float bq = dot(oc, rd);
        float cc = dot(oc, oc) - rad * rad;
        float disc = bq * bq - cc;
        if (disc < 0.0) {
            continue;
        }
        float sq = sqrt(disc);
        float cs0 = max(-bq - sq, uZNear);
        float cs1 = -bq + sq;
        if (cs1 <= cs0) {
            continue;
        }
        // 与本 froxel 的 z 段 [zk, zk1] 求交。
        float a = max(cs0, zk);
        float b = min(cs1, zk1);
        if (b <= a) {
            continue;
        }
        float len = b - a;
        vec3  p = ro + (0.5 * (a + b)) * rd;   // 段中点采样剖面
        float rr = length(p - c);
        float wgt = ProfileAtten(atten, rr / rad);
        if (wgt <= 0.0) {
            continue;
        }
        float dtau = inten * wgt * len;
        // 内散射源 L_in：step1 = base 发射色（不采样任何光源）；
        // step2 = col·(ambient + 太阳·强度·相位·CSM 阴影)。
        vec3 Lin = col;
        if (uSunEnable != 0) {
            float sh = CsmShadow(p, length(p - ro));
            float ph = SunPhase(rd);
            Lin = col * (uAmbientColor * uAmbientIntensity
                         + uSunColor * uSunIntensity * ph * sh);
        }
        vec3 Sleaf = Lin * (1.0 - exp(-dtau));
        acc = OverCompose(acc, vec4(dtau, Sleaf));   // over 合成（团无序 ⇒ 可任意序）
    }
    oColor = acc;
}
)glsl";

// ── 趟 2：reduce —— 金字塔一级（每柱边长 uLevelSide，由上一级 4 合 1）──
// 输出 cell m = 上一级 cell 4m..4m+3（z 序）的 over 合成 = 覆盖 4^level 个切片。
inline constexpr const char* kFireFogReduceBody = R"glsl(

uniform sampler2D uSrcLevel;
uniform int uLevelSide;   // 本级每柱 texel 边长 S_j
uniform int uSrcSide;     // 上一级每柱 texel 边长 S_{j-1} = 2·S_j

out vec4 oColor;   // (τ, S.rgb)

void main() {
    ivec2 frag = ivec2(gl_FragCoord.xy);
    int tile_col = frag.x / uLevelSide;
    int tile_row = frag.y / uLevelSide;
    int mx = frag.x - tile_col * uLevelSide;
    int my = frag.y - tile_row * uLevelSide;
    int m = my * uLevelSide + mx;              // 本级 cell 在柱内的索引
    ivec2 origin = ivec2(tile_col * uSrcSide, tile_row * uSrcSide);

    vec4 acc = vec4(0.0);
    for (int c = 0; c < 4; ++c) {
        int p = m * 4 + c;                     // 上一级 4 个子 cell（z 序升）
        int px = p % uSrcSide;
        int py = p / uSrcSide;
        acc = OverCompose(acc, texelFetch(uSrcLevel, origin + ivec2(px, py), 0));
    }
    oColor = acc;
}
)glsl";

// ── 趟 3：scatter —— 沿 z 有序前缀（金字塔 + base-4 分解，O(log Nz)）──
// uLevels[0] = L1（inject，块大小 1）；uLevels[j] = 缩减级 j（每柱边长 uBlockSide>>j，
// 块大小 4^j）。n = k+1（累积覆盖切片 [0, n)）：把 n 按 base-4 分解，
// 级 j 取块 [ (n>>2j)&~3 , +((n>>2j)&3) )，**从大块到小块按 z 序** over 合成。
// 每级 ≤3 块 ⇒ 总 ≤3·(uLevelsCount+1) 次合成 = O(log Nz)。
inline constexpr const char* kFireFogScatterBody = R"glsl(

uniform sampler2D uLevels[6];   // [0]=L1（inject），[1..5]=缩减级（块 4^1..4^5）
uniform int uLevelsCount;        // 缩减级数 = log4(uNz)

out vec4 oColor;   // (τ_cum, S_cum)，覆盖 [z_near, z_{k+1})

void main() {
    ivec2 frag = ivec2(gl_FragCoord.xy);
    int tile_col = frag.x / uBlockSide;
    int tile_row = frag.y / uBlockSide;
    int ix = frag.x - tile_col * uBlockSide;
    int iy = frag.y - tile_row * uBlockSide;
    int k = iy * uBlockSide + ix;
    int n = k + 1;

    vec4 acc = vec4(0.0);   // 恒等

    // 级 5（块 64，覆盖 1024 切片；nz<1024 时该级不启用）
    if (uLevelsCount >= 5) {
        int side = uBlockSide >> 5;
        int first = (n >> 10) & ~3;
        int cnt = (n >> 10) & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[5], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    // 级 4（块 16，覆盖 256 切片）
    if (uLevelsCount >= 4) {
        int side = uBlockSide >> 4;
        int first = (n >> 8) & ~3;
        int cnt = (n >> 8) & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[4], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    // 级 3（块 4，覆盖 64 切片）
    if (uLevelsCount >= 3) {
        int side = uBlockSide >> 3;
        int first = (n >> 6) & ~3;
        int cnt = (n >> 6) & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[3], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    // 级 2（块 16，覆盖 16 切片）
    if (uLevelsCount >= 2) {
        int side = uBlockSide >> 2;
        int first = (n >> 4) & ~3;
        int cnt = (n >> 4) & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[2], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    // 级 1（块 4，覆盖 4 切片）
    if (uLevelsCount >= 1) {
        int side = uBlockSide >> 1;
        int first = (n >> 2) & ~3;
        int cnt = (n >> 2) & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[1], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    // 级 0 = L1（inject，块大小 1，覆盖 1 切片）
    {
        int side = uBlockSide;
        int first = n & ~3;
        int cnt = n & 3;
        for (int c = 0; c < cnt; ++c) {
            int b = first + c;
            acc = OverCompose(acc, texelFetch(
                uLevels[0], ivec2(tile_col * side + b % side, tile_row * side + b / side), 0));
        }
    }
    oColor = acc;
}
)glsl";

// ── 趟 4：composite —— 全屏查表 + z 向插值 + Nxy 双线性 → 就地混合 ──
inline constexpr const char* kFireFogCompositeBody = R"glsl(

uniform sampler2D uScatter;
uniform sampler2D uSceneDepthTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;
uniform float uZFar;
uniform float uZScale;     // = uNz / ln(uZFar/uZNear)（CPU 预算）
uniform int   uGridCols;   // Nxy 单元列数
uniform int   uGridRows;   // Nxy 单元行数

out vec4 oColor;   // (S.rgb, T)；配合 GL_ONE/GL_SRC_ALPHA：out = S + dst·T

// 读某 tile 的 scatter 列在「位置 ℓ（= Nz·ln(z/near)/ln(far/near)）」处的 (τ, S)。
// texel t 的自然位置 = t+1（覆盖 [z_near, z_{t+1}））；ℓ<1 与 identity 插值。
void SampleColumn(ivec2 tile_xy, float ell, out float tau, out vec3 S) {
    int t_hi = int(floor(ell));
    float fr = ell - float(t_hi);
    t_hi = clamp(t_hi, 0, uNz - 1);
    int t_lo = t_hi - 1;
    vec4 vhi = texelFetch(uScatter, ivec2(tile_xy.x + (t_hi % uBlockSide),
                                          tile_xy.y + (t_hi / uBlockSide)), 0);
    vec4 vlo = (t_lo >= 0)
        ? texelFetch(uScatter, ivec2(tile_xy.x + (t_lo % uBlockSide),
                                     tile_xy.y + (t_lo / uBlockSide)), 0)
        : vec4(0.0);   // identity：τ=0, S=0 ⇒ T=1, S=0
    tau = mix(vlo.x, vhi.x, fr);
    S   = mix(vlo.yzw, vhi.yzw, fr);
}

void main() {
    vec2 uv = vTexCoord;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pf = uInvVP * vec4(ndc, 1.0, 1.0);
    vec3 ro = uCamPos;
    vec3 rd = normalize(pf.xyz / pf.w - ro);

    // 像素场景深度 → 沿视线距离 → froxel 连续位置 ℓ。
    float dndc = texture(uSceneDepthTex, uv).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    float z = length(pw.xyz / pw.w - ro);
    z = clamp(z, uZNear, uZFar);
    float ell = uZScale * log(z / uZNear);

    // 4 邻 tile（双线性，按 tile 中心对齐）。Nxy 单元在屏幕上是 uTilePx 像素。
    vec2 px = gl_FragCoord.xy;
    float cu = px.x / float(uTilePx) - 0.5;
    float cv = px.y / float(uTilePx) - 0.5;
    int tx0 = int(floor(cu));
    int ty0 = int(floor(cv));
    float fu = cu - float(tx0);
    float fv = cv - float(ty0);

    float tau = 0.0;
    vec3  S = vec3(0.0);
    for (int dy = 0; dy < 2; ++dy) {
        int ty = clamp(ty0 + dy, 0, uGridRows - 1);
        float wy = (dy == 0) ? (1.0 - fv) : fv;
        for (int dx = 0; dx < 2; ++dx) {
            int tx = clamp(tx0 + dx, 0, uGridCols - 1);
            float wx = (dx == 0) ? (1.0 - fu) : fu;
            float w = wx * wy;
            float ctau;
            vec3  cS;
            SampleColumn(ivec2(tx * uBlockSide, ty * uBlockSide), ell, ctau, cS);
            tau += w * ctau;
            S   += w * cS;
        }
    }
    oColor = vec4(S, exp(-tau));
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_

// JPOV Fire-Fog — GLSL 着色器（froxel 屏幕空间摊销 Z 轴采样）
//
// 设计见 tools/jpov/docs/jpov_froxel_design.md。三趟（全部在主 FBO 尺寸）：
//   inject   （fire_fog_inject）    每个 texel = 某 froxel 的**局部** (τ, S)：按 tile 中心
//                                   视线，在 [z_k, z_{k+1}) 段内累加候选雾团的 Δτ 与 S_leaf。
//   scatter  （fire_fog_scatter）   每个 texel = 某 froxel 的**累积** (τ, S)：沿 z 做有序前缀
//                                   （over 合成；MVP 串行，金字塔 O(log Nz) 为后续优化）。
//   composite（fire_fog_composite） 每个像素读 4 邻 tile 的 scatter 列、按像素深度 z 做
//                                   切片间插值 + 双线性，输出 vec4(S.rgb, T) 就地混合。
//
// froxel 网格：屏幕 16×16 tile = 一个 froxel 柱，柱内 256 个像素承载 256 个 z 切片
//（指数分布，近密远疏）；texel 索引 k = iy*16 + ix（行主序）。
//
// 存储语义（inject 与 scatter 均为 RGBA32F）：RGBA = (τ, S.r, S.g, S.b)。
//   - inject 存该切片的**局部** (Δτ, S_leaf)，S_leaf = L_in·(1 − exp(−Δτ))（over 形式）。
//   - scatter 的 texel k 存**累积**「覆盖 [z_near, z_{k+1})」的 (τ, S)，其自然位置 = z_{k+1}。
// ⚠️ shader 里的 #define 常量必须与 fire_fog_renderer.h / fire_fog_lower.h 对应。

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

// ── froxel / tile 常量（与 fire_fog_renderer.h、fire_fog_lower.h 对应）──
#define TILE_SIZE           16
#define NZ                  (TILE_SIZE * TILE_SIZE)   // 每柱切片数 = 16*16 = 256
#define MAX_FOGS_PER_TILE    8
#define TEXELS_PER_TILE      2
#define FOG_INDEX_SENTINEL   255u

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
)glsl";

// ── 趟 1：inject —— 逐 froxel 局部 (τ, S) ──
inline constexpr const char* kFireFogInjectBody = R"glsl(

uniform sampler2D uTileFogIndices;
uniform sampler2D uFogBodyTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;          // froxel z 分布近端（米）
uniform float uZFar;           // froxel z 分布远端（米）
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
    int tile_col = clamp(frag.x / TILE_SIZE, 0, grid_cols - 1);
    int tile_row = clamp(frag.y / TILE_SIZE, 0, grid_rows - 1);
    int ix = frag.x - tile_col * TILE_SIZE;
    int iy = frag.y - tile_row * TILE_SIZE;
    int k = iy * TILE_SIZE + ix;

    // 本 froxel 的 z 区间 [z_k, z_{k+1})（指数分布）。
    float R = pow(uZFar / uZNear, 1.0 / float(NZ));
    float zk  = uZNear * pow(R, float(k));
    float zk1 = zk * R;

    // tile 中心视线（froxel = 整柱一个值，用 tile 中心而非像素自身视线）。
    vec2 center_px = vec2(float(tile_col * TILE_SIZE) + 0.5 * float(TILE_SIZE),
                          float(tile_row * TILE_SIZE) + 0.5 * float(TILE_SIZE));
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

    float tau = 0.0;
    vec3  S = vec3(0.0);
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
        // over 合成（本 froxel 内多团，顺序无关）。
        S = S + exp(-tau) * Sleaf;
        tau += dtau;
    }
    oColor = vec4(tau, S);
}
)glsl";

// ── 趟 2：scatter —— 沿 z 有序前缀（累积 (τ, S)）──
inline constexpr const char* kFireFogScatterBody = R"glsl(

uniform sampler2D uInject;   // 趟 1 输出（局部 (τ, S)）

out vec4 oColor;   // (τ_cum, S_cum)，覆盖 [z_near, z_{k+1})

void main() {
    ivec2 frag = ivec2(gl_FragCoord.xy);
    int tile_col = frag.x / TILE_SIZE;
    int tile_row = frag.y / TILE_SIZE;
    int ix = frag.x - tile_col * TILE_SIZE;
    int iy = frag.y - tile_row * TILE_SIZE;
    int k = iy * TILE_SIZE + ix;
    ivec2 base = ivec2(tile_col * TILE_SIZE, tile_row * TILE_SIZE);

    float tau = 0.0;
    vec3  S = vec3(0.0);
    // MVP：串行前缀。金字塔 L2/L3/L4 + base-4 分解（O(log Nz)）为后续优化。
    for (int kk = 0; kk <= k; ++kk) {
        ivec2 lp = ivec2(kk % TILE_SIZE, kk / TILE_SIZE);
        vec4 v = texelFetch(uInject, base + lp, 0);
        S = S + exp(-tau) * v.yzw;
        tau += v.x;
    }
    oColor = vec4(tau, S);
}
)glsl";

// ── 趟 3：composite —— 全屏查表 + z 向插值 + Nxy 双线性 → 就地混合 ──
inline constexpr const char* kFireFogCompositeBody = R"glsl(

uniform sampler2D uScatter;
uniform sampler2D uSceneDepthTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;
uniform float uZFar;

out vec4 oColor;   // (S.rgb, T)；配合 GL_ONE/GL_SRC_ALPHA：out = S + dst·T

// 读某 tile 的 scatter 列在「位置 ℓ（= Nz·ln(z/near)/ln(far/near)）」处的 (τ, S)。
// texel t 的自然位置 = t+1（覆盖 [z_near, z_{t+1}））；ℓ<1 与 identity 插值。
void SampleColumn(ivec2 tile_xy, float ell, out float tau, out vec3 S) {
    int t_hi = int(floor(ell));
    float fr = ell - float(t_hi);
    t_hi = clamp(t_hi, 0, NZ - 1);
    int t_lo = t_hi - 1;
    vec4 vhi = texelFetch(uScatter, ivec2(tile_xy.x + (t_hi % TILE_SIZE),
                                          tile_xy.y + (t_hi / TILE_SIZE)), 0);
    vec4 vlo = (t_lo >= 0)
        ? texelFetch(uScatter, ivec2(tile_xy.x + (t_lo % TILE_SIZE),
                                     tile_xy.y + (t_lo / TILE_SIZE)), 0)
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
    float ell = float(NZ) * log(z / uZNear) / log(uZFar / uZNear);

    ivec2 tex_sz = textureSize(uScatter, 0);
    int grid_cols = tex_sz.x / TILE_SIZE;
    int grid_rows = tex_sz.y / TILE_SIZE;

    // 4 邻 tile（双线性，按 tile 中心对齐）。
    vec2 px = gl_FragCoord.xy;
    float cu = px.x / float(TILE_SIZE) - 0.5;
    float cv = px.y / float(TILE_SIZE) - 0.5;
    int tx0 = int(floor(cu));
    int ty0 = int(floor(cv));
    float fu = cu - float(tx0);
    float fv = cv - float(ty0);

    float tau = 0.0;
    vec3  S = vec3(0.0);
    for (int dy = 0; dy < 2; ++dy) {
        int ty = clamp(ty0 + dy, 0, grid_rows - 1);
        float wy = (dy == 0) ? (1.0 - fv) : fv;
        for (int dx = 0; dx < 2; ++dx) {
            int tx = clamp(tx0 + dx, 0, grid_cols - 1);
            float wx = (dx == 0) ? (1.0 - fu) : fu;
            float w = wx * wy;
            float ctau;
            vec3  cS;
            SampleColumn(ivec2(tx * TILE_SIZE, ty * TILE_SIZE), ell, ctau, cS);
            tau += w * ctau;
            S   += w * cS;
        }
    }
    oColor = vec4(S, exp(-tau));
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_SHADER_H_

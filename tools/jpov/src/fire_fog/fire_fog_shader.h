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

// ── 团 tile 索引纹理布局常量（编译期，与 fire_fog_renderer.h 对应）──
#define MAX_FOGS_PER_TILE    8
#define TEXELS_PER_TILE      2
#define FOG_INDEX_SENTINEL   255u

// ── 点光源 tile 索引纹理布局常量（fire_fog 自持；与 fire_fog_renderer.h 对应）──
// 每个 Nxy 单元至多 8 个光源（与最大团数同值），每 texel RGBA 各 1 个 uint8 索引。
#define MAX_LIGHTS_PER_TILE    8
#define LIGHT_TEXELS_PER_TILE  2
#define MAX_TOTAL_LIGHTS       255
#define LIGHT_INDEX_SENTINEL   255u

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

// ══ 团属性纹理 uFogBodyTex 布局（RGBA32F；每团 FOG_TEXELS_PER_BODY 个 texel）══
// ⚠️ 与 fog_body.h 的布局表（kFogBodyCommonLayout / kRoundBoxUnionLayout / kFlameUnionLayout）
//    **逐项一致**（契约单测解析下列标记比对）。改布局两处一起改。scope=head 为公共头（所有 kind）。
#define FOG_TEXELS_PER_BODY 10
//@FOGLAYOUT head texel=0 x=kind y=sigma z=albedo_r w=albedo_g
//@FOGLAYOUT head texel=1 x=albedo_b y=emission_r z=emission_g w=emission_b
//@FOGLAYOUT roundbox texel=2 x=center_x y=center_y z=center_z w=bound_hx
//@FOGLAYOUT roundbox texel=3 x=bound_hy y=bound_hz z=axis0_x w=axis0_y
//@FOGLAYOUT roundbox texel=4 x=axis0_z y=axis1_x z=axis1_y w=axis1_z
//@FOGLAYOUT roundbox texel=5 x=axis2_x y=axis2_y z=axis2_z w=radius
//@FOGLAYOUT roundbox texel=6 x=atten y=reserved z=reserved w=reserved
//@FOGLAYOUT flame texel=2 x=cont_cx y=cont_cy z=cont_cz w=cont_hx
//@FOGLAYOUT flame texel=3 x=cont_hy y=cont_hz z=center_x w=center_y
//@FOGLAYOUT flame texel=4 x=center_z y=box_hx z=box_hy w=box_hz
//@FOGLAYOUT flame texel=5 x=wind_x y=wind_z z=lean w=bend
//@FOGLAYOUT flame texel=6 x=shell y=plume z=vigor w=expand
//@FOGLAYOUT flame texel=7 x=decay y=reserved z=reserved w=reserved
//@FOGLAYOUT volumetex texel=2 x=center_x y=center_y z=center_z w=bound_hx
//@FOGLAYOUT volumetex texel=3 x=bound_hy y=bound_hz z=axis0_x w=axis0_y
//@FOGLAYOUT volumetex texel=4 x=axis0_z y=axis1_x z=axis1_y w=axis1_z
//@FOGLAYOUT volumetex texel=5 x=axis2_x y=axis2_y z=axis2_z w=density_pow
//@FOGLAYOUT volumetex texel=6 x=temp_scale y=reserved z=reserved w=reserved

// 采样器种类（与 fog_body.h 的 FogFieldKind 一致）。
#define FOG_KIND_ROUNDBOX 0
#define FOG_KIND_FLAME 4
#define FOG_KIND_VOLUMETEX 5

uniform sampler2D uFogBodyTex;
// 共享 3D 火纹理（帧序列；R=density, G=temperature，均 ∈[0,1]）。kVolumeTex 采样它。
// 存储为 2D 数组纹理：layer = frame*nz + z；每层 = nx×(ny 行) 的一张 z 切片（行内 x 最快）。
uniform sampler2DArray uFireVolTex;
uniform int   uVolumeNY;      // 体积 y 维数
uniform int   uVolumeNZ;      // 体积 z 维数（每帧 z 切片数）
uniform int   uVolumeFrames;  // 帧数
uniform float uVolumeTime;    // 播放时间（单位：帧，含小数；调用方按 fps 推进）

// 「每团通用数据」d：公共头（所有 kind 含义固定）+ union 区（按 kind 解释）。
// 含义随 kind 大变（见设计文档 §10.3）；名字按字段而非下标，SampleXXX 里只读自己那段。
struct FogBodyData {
    int   kind;
    float sigma;
    vec3  albedo;
    vec3  emission;
    vec3  center;
    vec3  axis0;
    vec3  axis1;
    vec3  axis2;
    vec3  bound_half;    // kRoundBox：容器半尺寸（= 内盒半 size/2 + radius）
    float radius;        // kRoundBox：外侧球面圆角壳厚度
    float atten;         // kRoundBox：壳剖面枚举
    vec3  cont_center;   // kFlame：容器 AABB 中心（CPU 保守算出后打包）
    vec3  cont_half;     // kFlame：容器 AABB 半尺寸
    vec3  fuel_center;   // kFlame：燃料盒中心
    vec3  box_half;      // kFlame：燃料盒半尺寸（世界单位，MVP 轴对齐）
    vec2  wind_dir;      // kFlame：风【水平】方向（单位；无风时 0）
    float lean;          // kFlame：截面顺风线性平移量（m）
    float bend;          // kFlame：截面顺风二次平移量（m）
    float shell;         // kFlame：包裹壳厚（米）
    float plume;         // kFlame：羽流柱长（米）
    float vigor;         // kFlame：旺盛度（自发光放大）
    float expand;        // kFlame：羽流半径微扩系数
    float decay;         // kFlame：羽流强度衰减指数
    float density_pow;   // kVolumeTex：密度幂（>= 1，收薄）
    float temp_scale;    // kVolumeTex：温度→自发光整体放大
};

// 解码器：按团 index 从 uFogBodyTex 取出该团的通用数据 d。
// 公共头恒取（texel 0..1）；union 区按 kind 取（kRoundBox = texel 2..6）。未识别 kind 清零。
FogBodyData DecodeFogBody(int bi) {
    FogBodyData d;
    vec4 t0 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 0, 0), 0);
    vec4 t1 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 1, 0), 0);
    d.kind     = int(t0.x + 0.5);
    d.sigma    = t0.y;
    d.albedo   = vec3(t0.z, t0.w, t1.x);
    d.emission = t1.yzw;
    d.center = vec3(0.0);
    d.axis0 = vec3(1.0, 0.0, 0.0);
    d.axis1 = vec3(0.0, 1.0, 0.0);
    d.axis2 = vec3(0.0, 0.0, 1.0);
    d.bound_half = vec3(0.0);
    d.radius = 0.0;
    d.atten = 0.0;
    d.cont_center = vec3(0.0);
    d.cont_half = vec3(0.0);
    d.fuel_center = vec3(0.0);
    d.box_half = vec3(0.0);
    d.wind_dir = vec2(0.0);
    d.lean = 0.0;
    d.bend = 0.0;
    d.shell = 0.0;
    d.plume = 0.0;
    d.vigor = 0.0;
    d.expand = 0.0;
    d.decay = 0.0;
    d.density_pow = 1.0;
    d.temp_scale = 0.0;
    if (d.kind == FOG_KIND_ROUNDBOX) {
        vec4 g0 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 2, 0), 0);
        vec4 g1 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 3, 0), 0);
        vec4 g2 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 4, 0), 0);
        vec4 g3 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 5, 0), 0);
        vec4 g4 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 6, 0), 0);
        d.center      = g0.xyz;
        d.bound_half  = vec3(g0.w, g1.x, g1.y);
        d.axis0       = vec3(g1.z, g1.w, g2.x);
        d.axis1       = g2.yzw;
        d.axis2       = g3.xyz;
        d.radius      = g3.w;
        d.atten       = g4.x;
    } else if (d.kind == FOG_KIND_FLAME) {
        vec4 g0 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 2, 0), 0);
        vec4 g1 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 3, 0), 0);
        vec4 g2 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 4, 0), 0);
        vec4 g3 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 5, 0), 0);
        vec4 g4 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 6, 0), 0);
        vec4 g5 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 7, 0), 0);
        d.cont_center = g0.xyz;
        d.cont_half   = vec3(g0.w, g1.x, g1.y);
        d.fuel_center = vec3(g1.z, g1.w, g2.x);
        d.box_half    = vec3(g2.y, g2.z, g2.w);
        d.wind_dir    = vec2(g3.x, g3.y);
        d.lean        = g3.z;
        d.bend        = g3.w;
        d.shell       = g4.x;
        d.plume       = g4.y;
        d.vigor       = g4.z;
        d.expand      = g4.w;
        d.decay       = g5.x;
    } else if (d.kind == FOG_KIND_VOLUMETEX) {
        vec4 g0 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 2, 0), 0);
        vec4 g1 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 3, 0), 0);
        vec4 g2 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 4, 0), 0);
        vec4 g3 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 5, 0), 0);
        vec4 g4 = texelFetch(uFogBodyTex, ivec2(bi * FOG_TEXELS_PER_BODY + 6, 0), 0);
        d.center      = g0.xyz;
        d.bound_half  = vec3(g0.w, g1.x, g1.y);
        d.axis0       = vec3(g1.z, g1.w, g2.x);
        d.axis1       = g2.yzw;
        d.axis2       = g3.xyz;
        d.density_pow = g3.w;
        d.temp_scale  = g4.x;
    }
    return d;
}

// ── 命名访问器（公共层只经这些读 d，避免裸下标散落）──
int   FogKind(FogBodyData d)       { return d.kind; }
float FogSigma(FogBodyData d)      { return d.sigma; }
vec3  FogAlbedo(FogBodyData d)     { return d.albedo; }
vec3  FogEmission(FogBodyData d)   { return d.emission; }
vec3  FogCenter(FogBodyData d)     { return d.center; }
vec3  FogBoundHalf(FogBodyData d) { return d.bound_half; }    // 容器半尺寸（内盒半 + radius）
float FogRadius(FogBodyData d)     { return d.radius; }        // 外侧圆角壳厚度
float FogAtten(FogBodyData d)      { return d.atten; }
vec3  FogContCenter(FogBodyData d) { return d.cont_center; }    // kFlame：容器 AABB 中心
vec3  FogContHalf(FogBodyData d)   { return d.cont_half; }      // kFlame：容器 AABB 半尺寸
vec3  FogFuelCenter(FogBodyData d) { return d.fuel_center; }    // kFlame：燃料盒中心
vec3  FogBoxHalf(FogBodyData d)    { return d.box_half; }       // kFlame：燃料盒半尺寸
vec2  FogWindDir(FogBodyData d)     { return d.wind_dir; }        // kFlame：风【水平】方向（单位）
float FogLean(FogBodyData d)       { return d.lean; }           // kFlame：截面平移量（m）
float FogBend(FogBodyData d)       { return d.bend; }           // kFlame：截面平移量（m）
float FogShell(FogBodyData d)      { return d.shell; }          // kFlame：包裹壳厚
float FogPlume(FogBodyData d)      { return d.plume; }          // kFlame：羽流柱长
float FogVigor(FogBodyData d)      { return d.vigor; }          // kFlame：旺盛度
float FogExpand(FogBodyData d)     { return d.expand; }         // kFlame：半径微扩系数
float FogDecay(FogBodyData d)      { return d.decay; }          // kFlame：强度衰减指数
float FogDensityPow(FogBodyData d) { return d.density_pow; }   // kVolumeTex：密度幂
float FogTempScale(FogBodyData d)  { return d.temp_scale; }    // kVolumeTex：温度→自发光放大

// 圆角盒「外壳归一化距离」t：q = p 的盒局部坐标（世界单位，沿 OBB 轴）。
//   · 内盒内（各轴 |qᵢ| ≤ box_hᵢ）→ t = 0（无衰减，w = 1）。
//   · 内盒外、圆角壳半径 radius 内：t = d(到盒面距离)/radius ∈ (0,1]。
//   · radius = 0（硬边盒）：盒内 t=0、盒外 t=2（≥1 ⇒ w=0）。
// d = 盒 SDF = length(max(|q|−box_h, 0)) + min(maxᵢ(|qᵢ|−box_hᵢ), 0)。
float RoundBoxShellT(vec3 q, vec3 box_h, float radius) {
    vec3 e = abs(q) - box_h;
    float d = length(max(e, vec3(0.0))) + min(max(e.x, max(e.y, e.z)), 0.0);
    if (radius <= 1e-6) {
        return (d <= 0.0) ? 0.0 : 2.0;
    }
    return max(d, 0.0) / radius;
}

// 单轴 slab 裁剪：p = dot(axis, ro−center)、dd = dot(axis, rd)。无交 → false。
// 光线与该轴平行（|dd| 极小）时：在板外 ⇒ 无交；在板内 ⇒ 该轴不约束。
bool SlabAxis(float p, float dd, float he, inout float lo, inout float hi) {
    if (abs(dd) < 1e-8) {
        return abs(p) <= he;
    }
    float ta = (-he - p) / dd;
    float tb = ( he - p) / dd;
    if (ta > tb) {
        float tmp = ta;
        ta = tb;
        tb = tmp;
    }
    lo = max(lo, ta);
    hi = min(hi, tb);
    return true;
}

// 光线 × 团 OBB 容器的 slab 求交（世界空间）→ 区间 [t0,t1]（t 沿单位 rd 的距离）。
// 无交 → false。容器求交**类型无关**（唯一几何原语）；各采样器共用。
// Pre-condition: ax0/1/2 正交单位；he 各 > 0。
bool RayObb(vec3 center, vec3 ax0, vec3 ax1, vec3 ax2, vec3 he,
            vec3 ro, vec3 rd, out float t0, out float t1) {
    vec3 oc = ro - center;
    float lo = -1e30;
    float hi = 1e30;
    if (!SlabAxis(dot(ax0, oc), dot(ax0, rd), he.x, lo, hi)) { return false; }
    if (!SlabAxis(dot(ax1, oc), dot(ax1, rd), he.y, lo, hi)) { return false; }
    if (!SlabAxis(dot(ax2, oc), dot(ax2, rd), he.z, lo, hi)) { return false; }
    if (hi < lo) { return false; }
    t0 = lo;
    t1 = hi;
    return true;
}

// 采样结果（各类型 SampleXXX 的**统一返回**）：段 [t0,t1]（已与本 froxel 段 [zk,zk1] 相交）
// + 采样点 p + 该点的 (σ, albedo, emission)。命中 = hit 且 t1>t0 且 σ>0。
// 「3 个返回项」= sigma / albedo / emission（源项拆分，见设计文档 §10.2）。
struct FogSegment {
    bool  hit;
    float t0;
    float t1;
    vec3  p;
    float sigma;
    vec3  albedo;
    vec3  emission;
};

// 未命中段（恒等）：各采样器的提前返回 / 未知 kind 的兵底。
FogSegment FogNoHit() {
    FogSegment s;
    s.hit = false;
    s.t0 = 0.0;
    s.t1 = 0.0;
    s.p = vec3(0.0);
    s.sigma = 0.0;
    s.albedo = vec3(0.0);
    s.emission = vec3(0.0);
    return s;
}

// ── kRoundBox 采样器（唯一分支本体；**严格 O(1)**：求交 + 一次点求值，无循环/无迭代纹理依赖）──
// 容器 = 内盒 ⊕ 球(radius)（保守 OBB）；内盒无衰减，圆角壳区由 attenuation 塑形。
FogSegment SampleRoundBox(FogBodyData d, vec3 ro, vec3 rd,
                          float zk, float zk1, float znear) {
    FogSegment s = FogNoHit();

    vec3 bh = FogBoundHalf(d);       // 容器半尺寸（内盒半 + radius）
    float rad = FogRadius(d);        // 外侧圆角壳厚度
    // 内盒半尺寸 = 容器半 − radius；须 > 0。
    if (bh.x <= rad || bh.y <= rad || bh.z <= rad || rad < 0.0) { return s; }
    vec3 box_h = bh - vec3(rad);

    // 求交用容器 OBB：内盒 ⊕ 球(radius) ⊂ 该 OBB，壳外无场处由点求值吸收。
    float o0;
    float o1;
    if (!RayObb(FogCenter(d), d.axis0, d.axis1, d.axis2, bh, ro, rd, o0, o1)) { return s; }

    float a = max(o0, max(zk, znear));
    float b = min(o1, zk1);
    if (b <= a) { return s; }

    vec3 p = ro + (0.5 * (a + b)) * rd;   // 段中点采样
    vec3 loc = p - FogCenter(d);
    vec3 q = vec3(dot(d.axis0, loc), dot(d.axis1, loc), dot(d.axis2, loc));  // 世界单位局部坐标
    float tt = RoundBoxShellT(q, box_h, rad);
    float w = ProfileAtten(int(FogAtten(d) + 0.5), tt);
    float sig = FogSigma(d) * w;
    if (sig <= 0.0) { return s; }

    s.hit = true;
    s.t0 = a;
    s.t1 = b;
    s.p = p;
    s.sigma = sig;
    s.albedo = FogAlbedo(d);
    s.emission = FogEmission(d);
    return s;
}

// ── kFlame 采样器（燃料盒包裹燃烧；**严格 O(1)**，不含 3D 纹理）──
// 「低频形」= max(包裹壳, 羽流柱)：
//   · 包裹壳：燃料盒**整个表面外侧** [0, shell] 的一层，密度从表面 (w=1) 向外线性衰减到 0；盒内不着火。
//   · 羽流柱：盒沿**世界 +Y**（浮力恒向上）挤出——盒顶面以上 [0, plume] 的柱；横向随高度微扩 expand；
//     **风**把截面顺【水平风向】随高度平移（lean·s+bend·s²）；强度 (1−s)^decay 软顶。
//     浮力固定 +Y；火焰倾斜只由 wind 驱动（见命令层 FlameFog）。
// 容器 AABB = CPU 在 LowerFlameFog 里保守算出并打包（= 剪枝用同值）；此处仅求交，不重算公式。
FogSegment SampleFlame(FogBodyData d, vec3 ro, vec3 rd,
                       float zk, float zk1, float znear) {
    FogSegment s = FogNoHit();
    vec3 box_h = FogBoxHalf(d);
    vec3 fuel_c = FogFuelCenter(d);
    float shell = FogShell(d);
    float plume = FogPlume(d);
    float expand = FogExpand(d);
    float decay = FogDecay(d);
    if (box_h.x <= 0.0 || box_h.y <= 0.0 || box_h.z <= 0.0 || shell <= 0.0) { return s; }

    float o0;
    float o1;
    if (!RayObb(FogContCenter(d), vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0),
                FogContHalf(d), ro, rd, o0, o1)) {
        return s;
    }
    float a = max(o0, max(zk, znear));
    float b = min(o1, zk1);
    if (b <= a) { return s; }

    vec3 p = ro + (0.5 * (a + b)) * rd;   // 段中点采样
    vec3 q = p - fuel_c;                   // 燃料盒局部坐标（= 世界轴：盒轴对齐）

    // 包裹壳：盒表面外侧 [0,shell] 线性衰减（盒内不着火）；整块燃料完整包裹。
    vec3 e = abs(q) - box_h;
    float dbox = length(max(e, vec3(0.0))) + min(max(e.x, max(e.y, e.z)), 0.0);
    float w_shell = 0.0;
    if (dbox > 0.0 && dbox < shell) {
        w_shell = 1.0 - dbox / shell;
    }

    // 羽流柱：盒沿世界 +Y 挤出（盒顶面以上的柱；横向随高度微扩 + 顺风水平平移）。
    float w_plume = 0.0;
    if (plume > 0.0) {
        float h = q.y - box_h.y;              // 盒顶以上的高度（沿世界 +Y）
        if (h > 0.0 && h < plume) {
            float ss = h / plume;
            float bh_min = min(box_h.x, min(box_h.y, box_h.z));
            float delta = expand * ss * bh_min;                                   // 横向随高度微扩
            vec2 off = FogWindDir(d) * (FogLean(d) * ss + FogBend(d) * ss * ss);  // 顺风平移
            vec2 pq = vec2(q.x, q.z) - off;
            float viol = max(abs(pq.x) - (box_h.x + delta), abs(pq.y) - (box_h.z + delta));
            float feather = 0.35 * bh_min;
            float soft = clamp(1.0 - max(viol, 0.0) / max(feather, 1e-4), 0.0, 1.0);
            w_plume = soft * pow(max(1.0 - ss, 1e-6), decay);
        }
    }

    float w = max(w_shell, w_plume);
    float sig = FogSigma(d) * w;
    if (sig <= 0.0) { return s; }

    s.hit = true;
    s.t0 = a;
    s.t1 = b;
    s.p = p;
    s.sigma = sig;
    s.albedo = FogAlbedo(d);
    s.emission = FogEmission(d) * max(FogVigor(d), 0.0) * w;
    return s;
}

// ── kVolumeTex 采样器（共享 3D 火纹理；**严格 O(1)**：求交 + 一次 texture 3D 采样）──
// 盒（OBB）映射到纹理 [0,1]^3：density(tex.r)→消光、temperature(tex.g)→自发光。
// 盒外 / 数值越界不采样（视为 0）。
FogSegment SampleVolumeTex(FogBodyData d, vec3 ro, vec3 rd,
                           float zk, float zk1, float znear) {
    FogSegment s = FogNoHit();
    vec3 he = FogBoundHalf(d);
    if (he.x <= 0.0 || he.y <= 0.0 || he.z <= 0.0) { return s; }

    float o0;
    float o1;
    if (!RayObb(FogCenter(d), d.axis0, d.axis1, d.axis2, he, ro, rd, o0, o1)) { return s; }
    float a = max(o0, max(zk, znear));
    float b = min(o1, zk1);
    if (b <= a) { return s; }

    vec3 p = ro + (0.5 * (a + b)) * rd;   // 段中点采样
    vec3 loc = p - FogCenter(d);
    vec3 q = vec3(dot(d.axis0, loc), dot(d.axis1, loc), dot(d.axis2, loc));
    vec3 uvw = q / (2.0 * he) + vec3(0.5);   // 盒 → 纹理 [0,1]^3

    vec2 tex = vec2(0.0);
    if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
        // 帧插值（时间 → 两帧线性混合，循环）。
        float nf = float(max(uVolumeFrames, 1));
        float ft = uVolumeTime - floor(uVolumeTime / nf) * nf;
        int f0 = int(floor(ft));
        float tf = ft - float(f0);
        int f1 = (f0 + 1) - (f0 + 1 >= max(uVolumeFrames, 1) ? max(uVolumeFrames, 1) : 0);
        // z 切片插值（相邻两个 z → 两个图层）。
        int nz = max(uVolumeNZ, 1);
        int ny = max(uVolumeNY, 1);
        float zf = clamp(uvw.z, 0.0, 1.0) * float(max(nz - 1, 1));
        int z0 = int(floor(zf));
        int z1 = min(z0 + 1, max(nz - 1, 1));
        float tz = zf - float(z0);
        float vy = clamp(uvw.y, 0.0, 1.0) * float(max(ny - 1, 1));
        float inv_row = 1.0 / float(ny * nz);
        float v0 = (float(z0) * float(ny) + vy + 0.5) * inv_row;
        float v1 = (float(z1) * float(ny) + vy + 0.5) * inv_row;
        vec2 d0 = texture(uFireVolTex, vec3(uvw.x, v0, float(f0))).rg;
        vec2 d1 = texture(uFireVolTex, vec3(uvw.x, v1, float(f0))).rg;
        vec2 e0 = texture(uFireVolTex, vec3(uvw.x, v0, float(f1))).rg;
        vec2 e1 = texture(uFireVolTex, vec3(uvw.x, v1, float(f1))).rg;
        tex = mix(mix(d0, d1, tz), mix(e0, e1, tz), tf);
    }
    float dens = pow(clamp(tex.r, 0.0, 1.0), max(FogDensityPow(d), 1.0));
    float sig = FogSigma(d) * dens;
    if (sig <= 0.0) { return s; }

    s.hit = true;
    s.t0 = a;
    s.t1 = b;
    s.p = p;
    s.sigma = sig;
    s.albedo = FogAlbedo(d);
    s.emission = FogEmission(d) * tex.g * max(FogTempScale(d), 0.0);
    return s;
}

// 唯一 kind 分派入口：按 d.kind 选采样器（GLSL330 无函数指针，用 if 链展开）。
// 加一个 kind = 加一个分支（不动 L1/L2/ZDist，见设计文档 §10.4）。
FogSegment SampleFogSegment(FogBodyData d, vec3 ro, vec3 rd,
                            float zk, float zk1, float znear) {
    if (FogKind(d) == FOG_KIND_ROUNDBOX) {
        return SampleRoundBox(d, ro, rd, zk, zk1, znear);
    }
    if (FogKind(d) == FOG_KIND_FLAME) {
        return SampleFlame(d, ro, rd, zk, zk1, znear);
    }
    if (FogKind(d) == FOG_KIND_VOLUMETEX) {
        return SampleVolumeTex(d, ro, rd, zk, zk1, znear);
    }
    return FogNoHit();
}
)glsl";

// ── 趟 1：inject —— 逐 froxel 局部 (τ, S) ──
inline constexpr const char* kFireFogInjectBody = R"glsl(

uniform sampler2D uTileFogIndices;
uniform sampler2D uTileZRange;   // RG32F：每 tile 的保守 z 范围 (zmin, zmax)
uniform sampler2D uTileRayTex;   // RGBA32F：每 tile 中心视线方向（单位向量，CPU 预算）
uniform sampler2D uZSlicesTex;   // R32F：z 切片边界 z_0..z_Nz（CPU 预算）
uniform sampler2D uTileLightIndices;  // RGBA8：每 Nxy 单元的光源索引（fire_fog 自持网格）
uniform vec3  uCamPos;
uniform float uZNear;          // froxel z 分布近端（米）
uniform int   uTotalFogs;

// ── 点光源（tile culling；仅需内散射所需的 position/color/radius/intensity）──
struct FogLight {
    vec3 position;
    vec3 color;
    float radius;      // 衰减半径（线性，与 object3d 同）
    float intensity;   // 亮度标量（乘 color）
};
uniform FogLight uFogLights[MAX_TOTAL_LIGHTS];
uniform int uTotalLights;

// ── 光照（**始终开启**；无「无光照 / base 发射」链路）──
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

// Henyey-Greenstein 散射相位（相对光→视线方向）。mu=1 = 朝光源看（前向散射）。
float HgPhase(float mu, float g) {
    g = clamp(g, 0.0, 0.95);
    float denom = max(1.0 + g * g - 2.0 * g * mu, 1e-4);
    return (1.0 - g * g) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// 太阳项相位（× uSunGain；uSunDir 由 CPU 归一、rd 取自已归一的 tile 射线纹理 ⇒ 不必再 normalize）。
float SunPhase(vec3 rd) {
    return uSunGain * HgPhase(clamp(dot(rd, -uSunDir), -1.0, 1.0), uSunPhaseG);
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

    // 本 froxel 的 z 区间 [z_k, z_{k+1})（指数分布；边界由 CPU 预表，省去逐 texel pow）。
    float zk  = texelFetch(uZSlicesTex, ivec2(k, 0), 0).r;
    float zk1 = texelFetch(uZSlicesTex, ivec2(k + 1, 0), 0).r;

    // ★ 短路：本 tile 候选团的保守 z 范围是 [zr.x, zr.y]；切片若完全在其外 ⇒ 局部恒等
    //（省掉下方 ≤8 个团的射线-球求交 + CSM 采样）。空 tile（无候选）zr=(0,0) ⇒ 恒等。
    vec2 zr = texelFetch(uTileZRange, ivec2(tile_col, tile_row), 0).xy;
    if (zk1 <= zr.x || zk >= zr.y) {
        oColor = vec4(0.0);
        return;
    }

    // tile 中心视线（froxel = 整柱一个值，用 tile 中心而非像素自身视线）：
    // 方向由 CPU 预烘在 tile 射线纹理里（单位向量），省掉每 texel 两次 uInvVP 乘 + normalize。
    vec3 ro = uCamPos;
    vec3 rd = texelFetch(uTileRayTex, ivec2(tile_col, tile_row), 0).xyz;

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

    // 本 tile 的点光源候选（≤ MAX_LIGHTS_PER_TILE；fire_fog 自持网格，与团的网格同尺寸）。
    uint light_idx[MAX_LIGHTS_PER_TILE];
    int nl = 0;
    for (int t = 0; t < LIGHT_TEXELS_PER_TILE; ++t) {
        vec4 lpx = texelFetch(uTileLightIndices,
                              ivec2(tile_col * LIGHT_TEXELS_PER_TILE + t, tile_row), 0);
        uint lch[4];
        lch[0] = uint(lpx.r * 255.0 + 0.5) & 255u;
        lch[1] = uint(lpx.g * 255.0 + 0.5) & 255u;
        lch[2] = uint(lpx.b * 255.0 + 0.5) & 255u;
        lch[3] = uint(lpx.a * 255.0 + 0.5) & 255u;
        for (int e = 0; e < 4; ++e) {
            if (lch[e] != LIGHT_INDEX_SENTINEL && lch[e] < uint(uTotalLights)
                && nl < MAX_LIGHTS_PER_TILE) {
                light_idx[nl] = lch[e];
                ++nl;
            }
        }
    }

    vec4 acc = vec4(0.0);   // 局部 (τ, S)，本 froxel 内多团 over 合成（顺序无关）
    for (int j = 0; j < nf; ++j) {
        int bi = int(fog_idx[j]);
        // 解码器：按团 index 取「每团通用数据 d」（公共头 + 按 kind 的 union 区）。
        FogBodyData d = DecodeFogBody(bi);
        // 唯一 kind 分派：求交 + 点求值（皆移入采样函数），返回段 + (σ, albedo, emission)。
        FogSegment seg = SampleFogSegment(d, ro, rd, zk, zk1, uZNear);
        if (!seg.hit) {
            continue;
        }
        float len = seg.t1 - seg.t0;
        float dtau = seg.sigma * len;
        if (dtau <= 0.0) {
            continue;
        }
        vec3 p = seg.p;   // 段中点采样点
        // ── 光照（始终开启）：ambient（各向同性） + 太阳×CSM（×相位，god ray）
        //    + 本 tile 点光源（×相位）。──
        vec3 Llight = uAmbientColor * uAmbientIntensity;
        if (uHasSun != 0) {
            float sh = CsmShadow(p, length(p - ro));
            Llight += uSunColor * uSunIntensity * SunPhase(rd) * sh;
        }
        for (int k2 = 0; k2 < nl; ++k2) {
            int li = int(light_idx[k2]);
            vec3 Lp = uFogLights[li].position - p;
            float ldist = length(Lp);
            if (ldist >= uFogLights[li].radius) {
                continue;
            }
            float atten_l = 1.0 - ldist / uFogLights[li].radius;
            float mu = clamp(dot(rd, Lp / max(ldist, 1e-5)), -1.0, 1.0);
            Llight += uFogLights[li].color * uFogLights[li].intensity
                      * atten_l * HgPhase(mu, uSunPhaseG);
        }
        float cover = 1.0 - exp(-dtau);
        // 源项分两块：散射 = albedo⊙L_light×覆盖；自发光 = ε×长度×自吸收
        //（薄极限 ε·len ⇒ 与密度无关，**不乘 σ**）。
        vec3 Sleaf = seg.albedo * Llight * cover;
        float emis_f = (dtau > 1e-6) ? cover / dtau : 1.0;
        Sleaf += seg.emission * (len * emis_f);
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

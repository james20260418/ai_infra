// JPOV FireFog — 后端「团」（FogBody）数据模型
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §10。
//
// 一切雾火效果（火 / 烟 / 薄雾 / 浪花 / 冲击波 …）在**命令层**各是不同子 command，
// 但到**后端**统一降维成一种「团」= FogBody：
//   - 容器：3D 空间 OBB（可旋转 box）—— 普适，且对细长效果比球更紧（不浪费屏幕覆盖）；
//   - 采样器：FogFieldKind 决定「该点的消光 σ 与发射」怎么算（多种类，唯一分派点）；
//   - 其余（tile 剪枝 / ZDist 累加 / 末端积分）与 kind 无关（见设计文档 §10.4）。
//
// ⚠️ 本头文件只放**数据模型**（GL-free、可单测）；tile 剪枝与 pass 实现在
// fire_fog_renderer.{h,cc}（见设计文档 §10.6 的 M1a）。

#ifndef JPOV_SRC_FIRE_FOG_FOG_BODY_H_
#define JPOV_SRC_FIRE_FOG_FOG_BODY_H_

#include <cstdint>

#include "tools/jpov/interface/render_command.h"  // Vec3f / Color

namespace jpov {

// 3D 空间有向包围盒（OBB）—— 团的普适容器。
//
// 三轴正交单位向量；允许旋转以对齐效果的自然轴（火/烟柱竖向、薄雾贴地），从而更紧。
// 盒 = center ± Σ_i axis[i]·half_extent[i]。半径用 half_extent 表达（各 > 0）。
// 剪枝：投影 8 个角点到屏幕取其 AABB（= 最紧轴对齐界，O(8)）；任一角在近平面后时
// 保守全屏回退（见设计文档 §10.5）。
struct Obb {
    Vec3f center;                 // 世界系中心
    Vec3f axis[3];                // 三正交单位轴（axis[i] = 团局部第 i 轴的世界朝向）
    Vec3f half_extent;            // 三半轴长度（各 > 0）
};

// 采样器种类（「多种类」的核心，唯一 kind 分派点）。
//
// ⚠️ v1 只实现 kRoundBox；其余是**扩展点**（语义与字段映射见设计文档 §10.3），
// 加一个 kind 不动 tile 剪枝 / ZDist（只加一个 SampleField 分支）。
enum class FogFieldKind : uint8_t {
    kRoundBox        = 0,  // 圆角盒：OBB 容器 + 盒内按「圆角盒距离」衰减（替换早期点雾/球）
    kHeightSlab      = 1,  // 贴地垂直 slab + 2D 域扭曲滚动（地表薄雾）
    kNoiseVolume     = 2,  // fbm/curl 调制「密度 × 发射」（火 / 烟 / 蘑菇云）
    kShell           = 3,  // 薄壳（冲击波 / 爆燃环）
};

// 后端唯一「团」记录（POD；预定进缓冲纹理，texelFetch 随机访问）。
//
// 采样契约：SampleXXX(kind, params, world_pos) → (σ, albedo, emission)。几何 / 分段 / 抖动 /
// 光照 / ZDist 累加全部 kind 无关（见设计文档 §10.4）。params 是**超集**：按 kind 解释，
// 未用到的位为 0（若某 kind 参数爆表，再拆二级 params buffer）。
struct FogBody {
    Obb bound;               // 容器（同时 = 积分区间 [t0,t1] 的界）
    FogFieldKind kind;       // 采样器种类（唯一分派点）
    float sigma;             // 消光系数 σ_t（1/m，>= 0）
    Color albedo;            // 单散射反照率 a = σ_s/σ_t ∈ [0,1]（散射色，逐通道）
    Color emission;          // 自发光系数 ε（HDR 辐射亮度/米，>= 0）；与密度无关
    float params[8];         // 采样器参数（超集，按 kind 解释；见设计文档 §10.3）
};

// ─────────────────────────────────────────────────────────────────────────────
// 团属性纹理（uFogBodyTex）布局契约 —— CPU 打包 / GLSL 解码的**单一来源**
//
// 布局 = RGBA32F 的一维条带，每团占 kTexelsPerBody 个 texel；texelFetch 按下标随机访问。
// **定长 tagged union**：每团的 texel 数（stride）对所有 kind 一致；公共头（前 2 texel）
// 含义固定，其后是 union 区（按 kind 解释）。第一个字段 = kind（类型判别子）。
//
// ⚠️ 本表与 fire_fog_shader.h 里的 GLSL 布局（`//@FOGLAYOUT` 标记 + FOG_TEXELS_PER_BODY）
//    必须逐项一致 —— 无编译器强制 ⇒ 用契约单测（fire_fog_contract_test.cc）解析 GLSL 字符串
//    与本表比对。改布局：两处一起改，单测会抓漏。
// ─────────────────────────────────────────────────────────────────────────────
struct FogBodyLayoutEntry {
    int texel;          // texel 下标（0..kTexelsPerBody-1）
    int comp;           // 分量（0=x,1=y,2=z,3=w）
    const char* name;   // 字段名（与 GLSL //@FOGLAYOUT 的字段名一致）
};

namespace fog_layout {
inline constexpr int kTexelsPerBody = 10;   // 每团 texel 数（公共头 2 + union 8，留扩展余量）
inline constexpr int kCommonTexels = 2;     // 公共头 texel 数（所有 kind 含义固定）
}  // namespace fog_layout

// 公共头 + 当前唯一 kind（kRoundBox）的 union 字段表。字段名即 //@FOGLAYOUT 的 token。
inline constexpr FogBodyLayoutEntry kFogBodyLayout[] = {
    // ── 公共头（所有 kind）──
    {0, 0, "kind"},      {0, 1, "sigma"},     {0, 2, "albedo_r"},  {0, 3, "albedo_g"},
    {1, 0, "albedo_b"},  {1, 1, "emission_r"},{1, 2, "emission_g"},{1, 3, "emission_b"},
    // ── union：kRoundBox 几何 + 形状（texel 2..6）──
    //   bound_h* = 容器半尺寸（内盒半 size/2 + radius）；radius = 外侧圆角壳厚度；atten = 壳剖面
    {2, 0, "center_x"},  {2, 1, "center_y"},  {2, 2, "center_z"},  {2, 3, "bound_hx"},
    {3, 0, "bound_hy"},  {3, 1, "bound_hz"},  {3, 2, "axis0_x"},   {3, 3, "axis0_y"},
    {4, 0, "axis0_z"},   {4, 1, "axis1_x"},   {4, 2, "axis1_y"},   {4, 3, "axis1_z"},
    {5, 0, "axis2_x"},   {5, 1, "axis2_y"},   {5, 2, "axis2_z"},   {5, 3, "radius"},
    {6, 0, "atten"},     {6, 1, "reserved"},  {6, 2, "reserved"},  {6, 3, "reserved"},
};

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FOG_BODY_H_

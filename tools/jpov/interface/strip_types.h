// JPOV 半透明条带（Strip3D）接口层类型 —— GL-free，render_command 与 src/strip 共用
//
// 设计（Danis 2026-10-21 定）：一条带 = **固定 16 个采样点**的双轨（left / right），
// 几何每帧由用户给的点决定；一批条带共享材质，**整批合成一次 draw call**。
// 设计文档：tools/jpov/docs/jpov_strip3d_translucent_design.md
//
// 为什么是「16 点固定」而不是任意点数：
//   ① 所有条带共享同一套拓扑 → 一个静态索引缓冲 + 一次 glDrawElements 画完整批；
//   ② 纹理按「16 格」约定绘制（v = col / 15），素材分辨率与几何分辨率一致；
//   ③ 形状每帧可变但**顶点数不变** → 不需要分桶、不需要 instancing。
//
// 坐标系：点一律是**世界空间**，本层不做任何变换（旋转/缩放请调用方自己算进点里）。
//
// 顶点与 UV 约定（Danis 2026-10-21 定，与素材完全对齐）：
//   每条带展开成 32 个顶点 = 16 列 ×（right, left），列 i 对应第 i 个采样点：
//     right[i] → uv = (i / 15, 0)      left[i] → uv = (i / 15, 1)
//   即 **u 沿条带长度方向（s，图片从左到右）**、**v 横跨宽度（0 = right 边 / 1 = left 边）**；
//   四个角对应素材：左下 right0、左上 left0、右下 right15、右上 left15。
//   相邻两列构成 1 个 quad，共 15 个 quad。
//
// 纹理透明：纹理的 alpha 直接当覆盖度用（黑底美术素材应先把黑转成 alpha=0）。

#ifndef JPOV_INTERFACE_STRIP_TYPES_H_
#define JPOV_INTERFACE_STRIP_TYPES_H_

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

using Vec3f = geom::Vec3<float>;

// 条带的两种绘制风格（Danis 2026-10-21 定：**两个独立 pass，统一一个命令接口**）。
//
//   kPolyline（默认）：left/right 是**顶点** —— 相邻两列之间是**直线（弦）**，
//                       折角处能看出多边形。最便宜，适合辅助线 / 硬边。
//   kSmooth：          left/right 是**插值型控制点（knot）** —— 相邻两列之间是
//                       **C1 Hermite 曲线**，边缘按真实曲线裁形（fwidth 解析 AA）。
//
// **两种风格的物理契约完全相同**，用户不需要为切换做任何事：
//   ① 曲线/折线**严格经过**每一个 left[i] / right[i]；
//   ② 第 i 列的宽度**严格等于** |left[i] − right[i]|；段间由半宽矢量插值决定；
//   ③ 想改宽窄 → **移点**（不存在 width 参数）。
//
// kSmooth 内部会按局部曲率把多边形**保守外扩**再由 FS 裁回去（片元只能削不能补），
// 外扩量是渲染器从几何自动算的**实现细节**：不暴露参数、也不改变上面两条契约。
//
// ⭐ 法线来源**与风格绑定**（2026-10-22 Danis 定，契约、没有独立开关）：
//   kPolyline ⇒ 真折角：fresnel 用折面法线（屏幕导数 dFdx/dFdy，逐三角形恒定）；
//   kSmooth   ⇒ 真柔和：fresnel 用**重建曲面的解析法线**（dS × H，无折面）。
//   「几何柔和 + 法线折角」这种混搭**不提供** —— 它只会让边反出现与几何无关的亮带
//   （也就是 15 段圆环上那圈竖条），没有任何一种使用意图对应它。
enum class StripStyle {
    kPolyline = 0,
    kSmooth = 1,
};

// 每条带的采样点数（= 段数 + 1）。改这个值 = 改所有素材的「格数」约定，不要随意动。
inline constexpr int kStripSampleCount = 16;

// 一条条带的 per-instance 数据（纯 POD：可整块 memcpy，用户可自行管理序列）。
//
// 三个数组**下标语义一致**：下标 i 同时表示 left[i] / right[i] / alpha[i]。
// 只有前 point_count 个点参与绘制；其余列**塌缩到最后一个有效点**（零面积四边形，
// 不产生片元），因此不足 16 点时不需要调用方补齐。
struct Strip16 {
    // 两条轨：right = 中心线 − side·半宽、left = 中心线 + side·半宽
    // （side = normalize(cross(切向, up))；由调用方决定，本层只管画）。
    std::array<Vec3f, kStripSampleCount> left;    // 左轨（纹理 v = 1）
    std::array<Vec3f, kStripSampleCount> right;   // 右轨（纹理 v = 0）
    // 纵向（沿长度）alpha **乘子**，不是覆盖率 —— 最终 α 还要 × 纹理 α × 批次 tint.a
    // × 宽度剖面。拖尾淡出就写这里（头 1 → 尾 0）。范围 [0,1]。
    std::array<float, kStripSampleCount> alpha;

    // 实际有效点数，必须显式给 [2, kStripSampleCount]。
    // 不设默认值：默认 0 会让「忘了填」直接 crash（而不是悄悄画出一条空条带）。
    uint32_t point_count = 0;
};

// 固定字节数：用户会按序列 memcpy / 一次性上传，尺寸变化必须是显式决策。
static_assert(sizeof(Strip16) == 2 * kStripSampleCount * sizeof(Vec3f) +
                                    kStripSampleCount * sizeof(float) +
                                    sizeof(uint32_t),
              "Strip16 的布局变了（别偷偷加字段：用户会按固定 stride memcpy）");
static_assert(std::is_trivially_copyable_v<Strip16>,
              "Strip16 必须可整块 memcpy（用户会按序列批量处理）");
static_assert(std::is_standard_layout_v<Strip16>,
              "Strip16 必须是 standard layout");

// 一条带的逐点（沿长度）颜色：16 个 RGBA 乘子（下标 = 第几列，与 left/right 对齐）。
//
// 为什么是 `std::array` 而不是加进 Strip16：Strip16 是固定 POD（用户按 stride memcpy），
// 加字段 = 破坏契约；逐点颜色只在"要渐变"时才需要，做成可选的平行数组更便宜。
using StripPointColors = std::array<Color, kStripSampleCount>;

// 上传路径直接把整块当 float 数组传给 GL（16×4 float，RGBA32F 一行），布局不能变。
static_assert(sizeof(StripPointColors) == kStripSampleCount * 4 * sizeof(float),
              "StripPointColors 必须是 16×4 个连续 float");

// 混合集合（对应 WBO 的两套附件）。
//
//   kCoverage  —— 吸收介质（玻璃/膜/烟）：写覆盖集 accum/reveal，参与遮挡；
//   kEmissive  —— 发光介质（拖尾/光环/剑气）：写加法集，**不被遮挡衰减**，
//                 与背景直接相加（黑底纹理天然透明：贡献 = 色·α = 0）。
// 同一批只能选一种（附件不同 = 不同 pass），要混用就发两个 batch。
enum class StripBlend {
    kCoverage,
    kEmissive,
};

// 菲涅耳「廉价玻璃边反」参数（**批次级**，整批共享）。
//
// 为什么需要它：条带本来是不受光的纯色片（§7 有意为之），单靠 tint 只会像一层
// 有色薄膜 —— 正对视线时几乎看不见、掠射时应该有强反光，这个差别只有菲涅耳能给。
//
// 公式（Schlick 近似，逐片元求值）：
//   cosθ = |N · V|                      // 双面：法线朝里朝外都成立
//   F    = F0 + (1 − F0)·(1 − cosθ)^power
//   F    = clamp(F · strength, 0, 1)
//   color = mix(color, rim_color, F)    // 边 → 反射色
//   α     = mix(α, graze_alpha·逐点α, F) // 边 → 不透明（中心保持看得穿）
//
// 法线 N 的来源**由 batch.style 决定**（见 StripStyle 的绑定契约），
// 本结构里**没有**法线模式字段 —— 也不该有：「柔和/折角」是同一个使用意图。
//
// 没有任何字段带「特殊值语义」：不想开就**不设** optional（与 soft_depth_m 同一写法）。
struct StripFresnel {
    // Schlick 正入射反射率。玻璃 ≈ 0.04；水 ≈ 0.02；厚水晶/亚克力可给到 0.08~0.12。
    float f0 = 0.04f;
    // Schlick 指数：5 = 物理值；越小「反光边」越宽（偏塑料），越大越只在极掠射发亮。
    float power = 5.0f;
    // 强度乘子（1 = 物理量）。> 1 让边比物理更亮 —— 廉价玻璃的常见夸张手法。
    float strength = 1.0f;
    // 掠射处的目标覆盖率（0..1）。1 = 边完全实心；0.6 = 边也只是更亮一点。
    float graze_alpha = 1.0f;
    // 边的反射色（线性 HDR，通常略偏冷/偏亮）。F 越大越靠近它。
    Vec3f rim_color{0.85f, 0.93f, 1.0f};
};

// 一批条带：**同一次 draw call** 画完，因此以下参数整批共享。
//
// 为什么共享而不是 per-strip：draw call 的边界就是「材质/状态边界」。
// 想给不同条带不同纹理/颜色 → 发多个 batch（每个 batch 一次 draw）。
struct TranslucentStripBatch {
    // 纹理（TextureManager id）。0 = 不采样纹理，纯色（tint）条带。
    uint32_t texture_id = 0;

    // 整批颜色（线性 HDR）。rgb 与纹理相乘；a 是全局不透明度乘子。
    // 默认白色——写成字面量而不是 kColorWhite：本头是 GL-free 的类型层，
    // 单元测试不该为了一个常量去链接 pbr_material.cc。
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};

    // 混合集合（见 StripBlend）。默认覆盖集 = 吸收介质。
    StripBlend blend = StripBlend::kCoverage;

    // 软深度过渡距离（视空间米）。nullopt = **关闭**（硬判据：比不透明场景远
    // 超过 2cm 就丢弃）；有值时必须 > 0，按 `clamp((scene_z − z)/值, 0, 1)`
    // 线性淡出，消除条带与几何相交处的硬切边。
    std::optional<float> soft_depth_m;

    // **宽度方向（v）**的 alpha 剖面：knots[k] = 在 v = k/8 处的乘子（9 个 knot / 8 段）。
    // （§16.3 修正 UV 约定后，u 是长度方向、v 才是宽度方向；这里的注释在修正前写成了 u。）
    // 默认全 1 = 不衰减（硬边）。要让边缘柔和/单侧渐隐就在 FS 里改这里。
    // 在**片元**里逐像素求值（不是顶点插值），所以 4 段几何也能有平滑剖面。
    std::array<float, 9> width_profile = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                          1.0f, 1.0f, 1.0f, 1.0f};

    // 本批条带（非空才画）。
    std::vector<Strip16> strips;

    // 加法集的 **HDR 亮度增益**。nullopt = 1.0（原样，逐字节不变）。
    //
    // 为什么需要它：`tint` 各分量被校验限制在 [0,1]，纹理也 ≤ 1，所以没有这个旋钮时
    // 加法集贡献的上限就是 1.0 —— 发光拖尾/射流永远吃不到 bloom 与 tone map 的 HDR 余量
    // （tone map 之前的世界是 scene-referred，亮部该能到几倍）。
    // 只对 `blend == kEmissive` 有意义：给覆盖集设它会崩（"设了不生效"比没有更糟）。
    std::optional<float> additive_gain;

    // 走哪个 pass（见 StripStyle）。同一帧可混用；两者写进同一张 WBO 覆盖集目标，
    // 所以**命令顺序 = 前后顺序**，最后只 Composite 一次。
    StripStyle style = StripStyle::kPolyline;

    // 菲涅耳（见 StripFresnel）。nullopt = 关闭 —— 画面逐字节等于没有这个功能。
    std::optional<StripFresnel> fresnel;

    // **逐条带（per instance）颜色/不透明度乘子**。空 = 整批用 tint（默认，零开销）。
    //
    // 语义是**相乘**：某条带的最终色 = tint × strip_tints[i]，
    // 于是 tint 仍是「材质色 + 整批不透明度」，这里是「这个实例偏什么色、多透」。
    // 这也让「同一批里每条拖尾颜色不同」不必拆成多个 draw call。
    //
    // 为什么是**平行数组**而不是往 Strip16 里加字段：Strip16 是固定 POD
    // （用户按 stride memcpy / 存盘），加字段 = 破坏契约；平行数组对它是零侵入。
    //
    // 非空时必须与 strips 等长（ValidateStripBatch 会检查，长度不匹配 = 静默错色）。
    std::vector<Color> strip_tints;

    // **逐点（沿长度）颜色**：每条带 16 个 RGBA 乘子。空 = 不用（默认，零开销）。
    //
    // 与 `Strip16::alpha`（逐点淡出）互补：那个管"多透"，这个管"什么颜色"。
    // 一起用才做得出「白 → 黄 → 红、同时尾部淡出」的射流/拖尾。
    // 非空时必须与 strips 等长；列与列之间由硬件的双线性插值平滑过渡。
    std::vector<StripPointColors> strip_point_colors;
};

// 上传路径直接把 Color 当 4 个 float 传给 GL（RGBA32F 纹理），布局不能变。
static_assert(sizeof(Color) == 4 * sizeof(float), "Color 必须是紧凑的 4 float");

// 纯色批：不采样纹理，直接用 color 当条带颜色（color.a = 不透明度乘子）。
//
// 等价于 texture_id == 0 + tint == color，这里显式命名——调用方不用猜
// 「texture_id 给 0 到底是不画还是纯色」。
// 注意：仍受 width_profile 与逐点 alpha 调制（即纯色也支持柔边与纵向淡出）。
// blend / soft_depth_m / style 一并给，避免「建好纯色批之后忘了设」这种顺序陷阱
// （纯色批默认仍是覆盖集 + 折线 pass + 硬判据）。
inline TranslucentStripBatch MakeSolidStripBatch(
    const Color& color, std::vector<Strip16> strips,
    StripBlend blend = StripBlend::kCoverage,
    std::optional<float> soft_depth_m = std::nullopt,
    StripStyle style = StripStyle::kPolyline) {
    TranslucentStripBatch batch;
    batch.texture_id = 0;          // 不采样纹理 ⇒ 纯色
    batch.tint = color;
    batch.blend = blend;
    batch.soft_depth_m = soft_depth_m;
    batch.style = style;
    batch.strips = std::move(strips);
    return batch;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_STRIP_TYPES_H_

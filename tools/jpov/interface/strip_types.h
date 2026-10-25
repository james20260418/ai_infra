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
//
// ⭐ 数据分两层（2026-10-22 Danis 定，重构后）：
//   * **per instance**：几何 + 逐点 RGBA + 宽度剖面 —— 全在 `Strip16` 里，一个自洽 POD，
//     用户按一个 stride 序列化 / memcpy 即可；
//   * **per batch**：材质（纹理 / 混合集合 / fresnel）+ 场景交接（软深度）—— 「同一材质
//     × 上千实例」才是 batch 的意义，跨实例不同的东西一律在 Strip16 里。
//   这也是为什么**没有** batch 级 tint / 逐条带 tint / 加法增益：它们都是逐实例量的
//   退化特例，留着只会让「同一个颜色有三个来源」这种问题重新长出来。

#ifndef JPOV_INTERFACE_STRIP_TYPES_H_
#define JPOV_INTERFACE_STRIP_TYPES_H_

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

using Vec3f = geom::Vec3<float>;

// 条带的**段间插值方式**（2026-10-22 从 StripStyle 改名：名字要说明它控制的是几何）。
//
//   kPolyline（默认）：left/right 是**顶点** —— 相邻两列之间是**直线（弦）**，
//                       折角处能看出多边形（15 段就是 15 段）。最便宜，适合辅助线 / 硬边。
//   kSmooth：          left/right 是**插值型控制点（knot）** —— 相邻两列之间是
//                       **C1 Hermite 曲线**，边缘按真实曲线裁形（fwidth 解析 AA）。
//
// **两种方式的物理契约完全相同**，切换不需要调用方做任何事：
//   ① 曲线/折线**严格经过**每一个 left[i] / right[i]；
//   ② 第 i 列的宽度**严格等于** |left[i] − right[i]|；段间由半宽矢量插值决定；
//   ③ 想改宽窄 → **移点**（不存在 width 参数）。
//
// 怎么选：
//   * 硬边辅助线 / 极细条带 / 只需要「一条带」→ kPolyline（更快，且细条带上两者看不出差别）；
//   * 闭合曲面（玻璃圆环）、长而弯的拖尾、开 fresnel 的东西 → kSmooth
//     （折线 pass 的边反会沿 15 段露出分面亮带，见下方法线绑定）。
//
// 代价（kSmooth）：多一张控制点数据纹理（每列 3 texel）+ CPU 端算切线/外扩量；
//   **FS 成本与采样点数无关（O(1)）**，所以点数不是它的问题。
//
// kSmooth 内部会按局部曲率把多边形**保守外扩**再由 FS 裁回去（片元只能削不能补），
// 外扩量是渲染器从几何自动算的**实现细节**：不暴露参数、也不改变上面两条契约。
// 已知边界：外扩上限 8×（按最细一列算），比这更细的尖端可能缺一条亚像素。
//
// ⭐ 法线来源**与插值方式绑定**（2026-10-22 Danis 定，契约、没有独立开关）：
//   kPolyline ⇒ 真折角：fresnel 用折面法线（屏幕导数 dFdx/dFdy，逐三角形恒定）；
//   kSmooth   ⇒ 真柔和：fresnel 用**重建曲面的解析法线**（dS × H，无折面）。
//   「几何柔和 + 法线折角」这种混搭**不提供** —— 它只会让边反出现与几何无关的亮带
//   （也就是 15 段圆环上那圈竖条），没有任何一种使用意图对应它。
enum class StripInterpolation {
    kPolyline = 0,
    kSmooth = 1,
};

// 每条带的采样点数（= 段数 + 1）。改这个值 = 改所有素材的「格数」约定，不要随意动。
inline constexpr int kStripSampleCount = 16;

// 宽度（v）方向剖面的 knot 数：knots[k] 是 v = k/8 处的乘子（8 段 / 9 个 knot）。
inline constexpr int kStripProfileKnots = 9;

// 逐点颜色的**上限**（线性 HDR）。不是物理约束，是防御：超过它基本可以断定是单位搞错
// （比如把 0..255 的 8bit 颜色直接塞进来），而不是艺术意图。
inline constexpr float kMaxStripColorValue = 10000.0f;

// 逐点颜色的默认值 = 恒等乘子（白 + 不透明）。
// 这不是「特殊值语义」，而是乘法单位元：没填颜色时条带就是「纹理原色 / 纯白」，
// 与重构前 tint = 白色、alpha = 1 的行为一致。
inline constexpr std::array<Color, kStripSampleCount> kDefaultStripPointColors = [] {
    std::array<Color, kStripSampleCount> out{};
    for (int i = 0; i < kStripSampleCount; ++i) {
        out[static_cast<size_t>(i)] = Color{1.0f, 1.0f, 1.0f, 1.0f};
    }
    return out;
}();

// 宽度剖面默认值 = 全 1（硬边）。同样是乘法单位元：不填就是「不改宽度方向」。
inline constexpr std::array<float, kStripProfileKnots> kFlatWidthProfile = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

// 一条条带的**全部 per-instance 数据**（纯 POD：可整块 memcpy，用户可自行管理序列）。
//
// 三个数组**下标语义一致**：下标 i 同时表示 left[i] / right[i] / point_color[i]。
// 只有前 point_count 个点参与绘制；其余列**塌缩到最后一个有效点**（零面积四边形，
// 不产生片元），因此不足 16 点时不需要调用方补齐。
//
// 为什么颜色/剖面也在这里（2026-10-22 重构）：
//   它们是「这个实例长什么样」的一部分，和几何同源、同生命周期。以前放在 batch 上
//   （颜色还分成 batch tint + 逐条带 tint + 逐点颜色三层）导致同一个颜色有三个乘数，
//   用户必须同时记住三个地方；现在只有一层：**逐点 RGBA**。
struct Strip16 {
    // 两条轨：right = 中心线 − side·半宽、left = 中心线 + side·半宽
    // （side = normalize(cross(切向, up))；由调用方决定，本层只管画）。
    std::array<Vec3f, kStripSampleCount> left;    // 左轨（纹理 v = 1）
    std::array<Vec3f, kStripSampleCount> right;   // 右轨（纹理 v = 0）

    // **逐点 RGBA**（下标 = 第几列，与 left/right 对齐）：
    //   rgb = 线性 HDR 颜色乘子：与纹理色、fresnel 边反色相乘。
    //         **允许 > 1**（scene-referred）：加法集（发光）靠它拿到 bloom / tone map
    //         的 HDR 余量 —— 不需要额外的「增益」旋钮（见 TranslucentStripBatch 的说明）。
    //   a   = 沿长度（u）方向的不透明度乘子（不是纹理 alpha）：最终 α 还要 × 纹理 α
    //         × 宽度剖面。拖尾淡出就写这里（头 1 → 尾 0）。范围 [0,1]。
    // 默认 = 白 + 不透明（乘法单位元，见 kDefaultStripPointColors）。
    std::array<Color, kStripSampleCount> point_color = kDefaultStripPointColors;

    // **宽度（v）方向**的 alpha 剖面：knots[k] = 在 v = k/8 处的乘子（9 knot / 8 段）。
    // 默认全 1 = 不衰减（硬边）。要让边缘柔和 / 单侧渐隐就改这里，例如
    // `{0, .35, .8, 1, 1, 1, .8, .35, 0}` = 两个软边 + 实心芯。
    // 在**片元**里逐像素求值（不是顶点插值），所以 4 段几何也能有平滑剖面；
    // 每个 knot ∈ [0,1]（至少一个 > 0，否则整条不可见 → Validate 崩）。
    std::array<float, kStripProfileKnots> width_profile = kFlatWidthProfile;

    // 实际有效点数，必须显式给 [2, kStripSampleCount]。
    // 不设默认值：默认 0 会让「忘了填」直接 crash（而不是悄悄画出一条空条带）。
    uint32_t point_count = 0;
};

// 固定字节数（384 + 256 + 36 + 4 = 680）：用户会按序列 memcpy / 一次性上传，
// 尺寸变化必须是显式决策 —— 改这个 assert 之前先问「用户存的盘要不要迁移」。
static_assert(sizeof(Strip16) == 2 * kStripSampleCount * sizeof(Vec3f) +
                                    kStripSampleCount * sizeof(Color) +
                                    kStripProfileKnots * sizeof(float) +
                                    sizeof(uint32_t),
              "Strip16 的布局变了（别偷偷加字段：用户会按固定 stride memcpy）");
static_assert(std::is_trivially_copyable_v<Strip16>,
              "Strip16 必须可整块 memcpy（用户会按序列批量处理）");
static_assert(std::is_standard_layout_v<Strip16>,
              "Strip16 必须是 standard layout");

// 混合集合（对应 WBO 的两套附件）。**这是一个 batch 的物理材质属性**，不是开关：
// 选错 = 东西的物理行为错了，而不只是"差点颜色"。
//
//   kCoverage —— **吸收 / 遮挡介质**（玻璃、薄膜、烟雾、花纹屏）。
//       · 写覆盖集（accum + reveal），**参与遮挡**：背后的东西会被这层按 α 混掉；
//       · 逐片元 α（覆盖率）**同时**决定「注入多少颜色」与「挡掉多少背景」：
//           accum += color·α·w ，reveal *= (1−α)
//           解析：C = Σ(color·α·w)/Σ(α·w) ，cov = 1 − Π(1−α)
//           单层时退化成标准 over：out = color·α + dst·(1−α)
//         （物理解释：一层薄片的等效光学厚度 τ = −ln(1−α) ≈ α（薄片极限），
//           多层 T = Π(1−αᵢ)。**没有独立的 sigma 参数**，α 就是全部。）
//       · color.rgb > 1 在这里表示「膜本身是 HDR 亮的」（会被 bloom 捡到），
//         **不改变遮挡量**；多层叠加时它会经 WBO 权重函数影响层间相对权重（单层无影响）。
//       · 选它：正对视线时"看得穿、有颜色"、掠射时变实 —— 也就是玻璃/膜的观感。
//
//   kEmissive —— **发光介质**（拖尾、光环、剑气、火焰射流）。
//       · 写加法集：`add += color·α·fade`，最终 `out = dst + add`，**不被遮挡衰减**；
//       · α 在这里只是"注入量的一个乘子"（形状淡出 / 纹理 alpha 仍然要靠它），
//         纯亮度可以直接用 color.rgb > 1 表达，**不需要额外的增益旋钮**；
//       · 黑底纹理天然透明：color·α = 0（所以素材把黑色转成 alpha=0 即可）；
//       · 选它：它**照亮**背景而不是挡住背景。把它误设成 kCoverage，画面会变成
//         "一层有颜色的膜"（暗、且挡后面的东西）；把玻璃误设成 kEmissive 会失去遮挡、
//         整块玻璃浮在物体前面发光 —— 两个方向都是明显错误，但不是崩溃。
//
// 集合内部**与绘制顺序无关**（accum 是加法、reveal 是乘法）；集合之间固定
// **先覆盖集、后加法集** ⇒ 加法光不被遮罩衰减（这是单层 OIT 的固有近似，见 docs §4.2）。
// 一个 batch 只能选一种集合；「玻璃 + 加法高光」要发**两个** batch。
enum class StripBlend {
    kCoverage,
    kEmissive,
};

// 菲涅耳「廉价玻璃边反」参数（**批次级**，整批共享）。
//
// 为什么需要它：条带本来是不受光的纯色片（§7 有意为之），单靠颜色只会像一层
// 有色薄膜 —— 正对视线时几乎看不见、掠射时应该有强反光，这个差别只有菲涅耳能给。
//
// 公式（Schlick 近似，逐片元求值）：
//   cosθ = |N · V|                      // 双面：法线朝里朝外都成立
//   F    = F0 + (1 − F0)·(1 − cosθ)^power
//   F    = clamp(F · strength, 0, 1)
//   color = mix(color, rim_color, F)          // 边 → 反射色
//   α     = mix(α, graze_alpha, F)            // 边 → 不透明（中心保持看得穿）
//
// ⚠️ 边反**不乘任何 α 乘子**（逐点 alpha / 纹理 alpha / 剖面 alpha）：反射是独立旋钮 ——
//   薄膜 α=0.12 也该有实心亮边，这正是 fresnel 存在的理由。
//   2026-10-22 重构后「不透明度」与「沿长度淡出」共用逐点颜色的 alpha 通道，两者不可分；
//   乘上去会让 0.12 的玻璃壳完全没有亮边。代价（已知限制）：开 fresnel 的条带不要在
//   长度方向做淡出，否则淡出段的边反会保留。
//
// 法线 N 的来源**由 batch.geometry 决定**（见 StripInterpolation 的绑定契约），
// 本结构里**没有**法线模式字段 —— 也不该有：「柔和/折角」是同一个使用意图。
//
// 没有任何字段带「特殊值语义」：不想开就**不设** optional（与 soft_depth_m 同一写法）。
struct StripFresnel {
    // Schlick 正入射反射率。玻璃 ≈ 0.04；水 ≈ 0.02；厚水晶/亚克力可给到 0.08~0.12。
    float f0 = 0.04f;
    // Schlick 指数：5 = 物理值；越小「反光边」越宽（偏塑料），越大越只在极掠射发亮。
    // 恰好 5 时渲染器走五次乘法特化（不调 pow）；对外语义不变，浮点末位可能差 1 LSB。
    float power = 5.0f;
    // 强度乘子（1 = 物理量）。> 1 让边比物理更亮 —— 廉价玻璃的常见夸张手法。
    float strength = 1.0f;
    // 掠射处的目标覆盖率（0..1）。1 = 边完全实心；0.6 = 边也只是更亮一点。
    float graze_alpha = 1.0f;
    // 边的反射色（线性 HDR，通常略偏冷/偏亮）。F 越大越靠近它。
    Vec3f rim_color{0.85f, 0.93f, 1.0f};
};

// 条带「消费天光」—— **覆盖集（over）专用**，批次级参数（2026-10-25 Danis 定）。
//
// 为什么需要它：到这一版为止条带是**完全不受光**的 —— 逐点颜色就是最终色，与天光 / 太阳
// 无关。玻璃缸、雾里的薄膜这类东西一旦放进真实光照场景，亮度就会不对。
//
// 公式（与雾的合成**同构**，逐片元）：
//   C = point_color.rgb · tex.rgb                     // ① 自发光 / 表面色（语义与不开光照时相同）
//     + injection · skylight(N)                       // ② 被吸收的背景光按**天光**补注入
//   skylight(N) = tricolor_lerp(N.y) · ambient_scale  // 上→天、水平→天际线、下→地（N.y 分段线性）
//               + sun_color · sun_intensity · |N·L| · csm  // 薄片双面：两侧都吃太阳
//                                                  ↑ CSM 阴影因子 [0,1]（2026-10-25 加）
//   α = 覆盖率链（不变）
//   L_out = α·C + (1−α)·L_bg                          // WBO over 合成（与雾的 L·(1−α)+L_fog·α 同构）
//
// **太阳项吃 CSM 阴影，且与天光同一个开关**（Danis 2026-10-25：「CSM 被 over 类型 strip 消费，
// 开关和天光消费一起，这俩绑定」）：有 `sky_light` 就采样引擎的级联阴影贴图调制太阳项，
// **没有独立的阴影开关**；天光项（ambient）不乘阴影 —— 环境光不该被直射光的影子遮住。
// 条带自身**不参与**阴影生成（不写 shadow map）：那需要给半透明片单开一次 shadow pass，
// 收益与代价不成比例，Danis 明确先不做。采样是**单级联单次采样**（无 PCF，与 fire_fog 同取舍）。
//
// 三个关键约定：
//   * **注入量自然随不透明度缩放**（C 随后要乘 α）⇒ 越不透明补得越多、越透补得越少，
//     就是雾里的 `L_fog·α`；所以**没有**单独的「吸收系数」参数（有它会与 α 双重计数）。
//   * `point_color.rgb` 的语义**不变**：以前它是最终色，开天光后它是「自发光 + 表面色」，
//     天光是**新增**的一项 —— 不想要自发光就把逐点颜色调暗。
//   * 只允许覆盖集：加法集本身就是发光介质（其注入量已是 色·α），再叠天光会让 HDR 语义混乱
//     ⇒ `ValidateStripBatch` 对 `kEmissive + sky_light` 直接 CHECK 崩。
//
// 代价 / 性能：渲染器为「开天光」多编译一套 program（折线、平滑各一）。
// **不开天光的批仍走原来那两个 program，指令数与改动前逐字节相同**（空实现被内联消掉，
// 由 scene gold 逐字节不变反向证明，见设计文档 §19.13）。
struct StripSkyLight {
    // 天光三色，顺序**固定** [天(zenith), 天际线(horizon), 地(ground)]。
    // 默认值 = 中性白天天空，**只用于快速试手**；正式使用请显式给（通常就是
    // `SkyCommand::AmbientTricolor()`），否则「缸壁亮不亮」跟场景天光会脱节。
    // （与 AmbientLight::tricolor 同约定；通常直接取 `SkyCommand::AmbientTricolor()`）。
    // 每项是线性 HDR 颜色；下标含义不能对调。
    std::array<Color, 3> tricolor = {Color{0.45f, 0.58f, 0.85f, 1.0f},
                                     Color{0.75f, 0.82f, 0.92f, 1.0f},
                                     Color{0.16f, 0.15f, 0.13f, 1.0f}};
    // 天光整体亮度倍率（≈ `AmbientLight::intensity`；只缩放天光，不碰太阳）。
    float ambient_scale = 1.0f;
    // 太阳方向：**指向太阳**的世界空间单位向量（与 `SkyCommand::sun_dir` 同义）。
    Vec3f sun_direction{0.0f, 1.0f, 0.0f};
    // 太阳直射色（线性 HDR）。通常取 `SkyCommand::SunDirectionalColor()`。
    Vec3f sun_color{1.0f, 1.0f, 1.0f};
    // 太阳直射强度；0 = 无直射（只吃天光）。通常取 `SunDirectionalIntensity()`。
    float sun_intensity = 0.0f;
    // 吸收→注入比例（0..1）：0 = 纯吸收（条带只遮挡、自己不亮），1 = 吸收多少补多少。
    // 它表达的是**物理比例**，不是亮度旋钮 —— 想要更亮就放大 tricolor / ambient_scale。
    float injection = 1.0f;
};

// ── 一条条带的最终像素（本节是**唯一权威**，FS 与设计文档都以它为准）──
//
// 逐片元输入：(u, v) 由几何插值得到；(rgb, alpha_pt) 由逐点颜色按 u 采样得到
// （折线 pass 走 per-instance 纹理的硬件双线性，列上是精确值；平滑 pass 直接在控制点
// 数据纹理里按段内参数 t* 做 mix —— 两者数学等价，见设计文档 §19.12）；
// N 由 `geometry` 决定（见 StripInterpolation）。
//
//   color = point_color(u).rgb                    （逐点颜色，允许 HDR）
//         × texture(uTex, uv).rgb                 （有纹理时）
//         → [sky_light] color += injection · skylight(N)   （仅覆盖集，见 StripSkyLight）
//         → [fresnel]   color = mix(color, rim_color, F)
//
//   a     = point_color(u).a                      （逐点纵向不透明度）
//         × texture.a                             （有纹理时）
//         → [fresnel]  a = mix(a, graze_alpha, F)   （不乘任何 α，见 StripFresnel）
//         → a *= width_profile(v)                 （9 knot，**逐片元**求值）
//         → a *= 裁形覆盖度                        （仅 kSmooth：解析 AA）
//         → [soft depth] a *= clamp((scene_z − z)/m, 0, 1)；
//           未开软深度时 z 比 scene_z 远超过 2cm 直接 discard
//
//   早退：**注入量** ≤ 1/255 的片元直接丢弃 —— 覆盖集判 α（a·fade ≤ a ⇒ 等价），
//         加法集判 色·α（颜色是 HDR 时不能只判 α，否则极亮淡尾会被丢掉）。
//
//   混合集合（见 StripBlend，公式里的注入量**逐字**对应 FS 的写入）：
//     kCoverage: accum += color·a·w 、reveal = a        （解析后 over 合成）
//     kEmissive: add   += color·a·fade                  （直接求和，不衰减背景）
//
// 全部乘子都在**线性 HDR、tone map 之前**；顺序里不可交换的那几步（sky_light / fresnel /
// profile / 裁形 AA / soft depth）按上面写死，改顺序就是改观感，必须同步本文档。
//
// 一批条带：**同一次 draw call** 画完，因此以下参数整批共享。
//
// 为什么共享而不是 per-strip：draw call 的边界就是「材质/状态边界」。
// 想给不同条带不同纹理/混合集合 → 发多个 batch（每个 batch 一次 draw）。
struct TranslucentStripBatch {
    // 纹理（TextureManager id）。0 = 不采样纹理，颜色完全由逐点颜色决定。
    uint32_t texture_id = 0;

    // 混合集合（见 StripBlend）。默认覆盖集 = 吸收介质。
    StripBlend blend = StripBlend::kCoverage;

    // 软深度过渡距离（视空间米）。nullopt = **关闭**（硬判据：比不透明场景远
    // 超过 2cm 就丢弃）；有值时必须 > 0，按 `clamp((scene_z − z)/值, 0, 1)`
    // 线性淡出，消除条带与几何相交处的硬切边。
    std::optional<float> soft_depth_m;

    // 走哪条 pass（见 StripInterpolation）。同一帧可混用；两者写进同一张 WBO 目标，
    // 所以**命令顺序 = 前后顺序**，最后只 Composite 一次。
    StripInterpolation geometry = StripInterpolation::kPolyline;

    // 菲涅耳（见 StripFresnel）。nullopt = 关闭 —— 画面逐字节等于没有这个功能。
    std::optional<StripFresnel> fresnel;

    // 天光消费（见 StripSkyLight）。nullopt = 关闭 —— 走原来那两个 program，
    // **指令数与画面逐字节等于没有这个功能**。只允许 `blend == kCoverage`。
    std::optional<StripSkyLight> sky_light;

    // 本批条带（非空才画）。每条带的颜色/不透明度/宽度剖面都在 Strip16 里 ——
    // 同一批里每条拖尾颜色不同、边缘软硬不同，都不需要拆成多个 draw call。
    std::vector<Strip16> strips;
};

// 纯色批：texture_id 给 0，再把每条带的逐点颜色全填成同一个值
// （`strip.point_color.fill(color)`）—— 逐点颜色就是唯一颜色来源，没有第二层 tint。
// 注意：仍受该条带 width_profile 调制（纯色也支持柔边与纵向淡出）。

}  // namespace jpov

#endif  // JPOV_INTERFACE_STRIP_TYPES_H_

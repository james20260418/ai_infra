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

// 每条带的采样点数（= 段数 + 1）。改这个值 = 改所有素材的「格数」约定，不要随意动。
inline constexpr int kStripSampleCount = 16;

// 一条条带的 per-instance 数据（纯 POD：可整块 memcpy，用户可自行管理序列）。
//
// 三个数组**下标语义一致**：下标 i 同时表示 left[i] / right[i] / alpha[i]。
// 只有前 point_count 个点参与绘制；其余列**塌缩到最后一个有效点**（零面积四边形，
// 不产生片元），因此不足 16 点时不需要调用方补齐。
struct Strip16 {
    std::array<Vec3f, kStripSampleCount> left;    // 左轨（纹理 u = 1）
    std::array<Vec3f, kStripSampleCount> right;   // 右轨（纹理 u = 0）
    std::array<float, kStripSampleCount> alpha;   // 纵向 alpha 乘子，[0, 1]

    // 实际有效点数，必须显式给 [2, kStripSampleCount]。
    // 不设默认值：默认 0 会让「忘了填」直接 crash（而不是悄悄画出一条空条带）。
    uint32_t point_count = 0;
};

static_assert(std::is_trivially_copyable_v<Strip16>,
              "Strip16 必须可整块 memcpy（用户会按序列批量处理）");
static_assert(std::is_standard_layout_v<Strip16>,
              "Strip16 必须是 standard layout");

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

    // 宽度方向的 alpha 剖面：knots[k] = 在 u = k/8 处的乘子（9 个 knot / 8 段）。
    // 默认全 1 = 不衰减（硬边）。要让边缘柔和/单侧渐隐就在 FS 里改这里。
    // 在**片元**里逐像素求值（不是顶点插值），所以 4 段几何也能有平滑剖面。
    std::array<float, 9> width_profile = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                          1.0f, 1.0f, 1.0f, 1.0f};

    // 本批条带（非空才画）。
    std::vector<Strip16> strips;
};

// 纯色批：不采样纹理，直接用 color 当条带颜色（color.a = 不透明度乘子）。
//
// 等价于 texture_id == 0 + tint == color，这里显式命名——调用方不用猜
// 「texture_id 给 0 到底是不画还是纯色」。
// 注意：仍受 width_profile 与逐点 alpha 调制（即纯色也支持柔边与纵向淡出）。
inline TranslucentStripBatch MakeSolidStripBatch(const Color& color,
                                                 std::vector<Strip16> strips) {
    TranslucentStripBatch batch;
    batch.texture_id = 0;
    batch.tint = color;
    batch.strips = std::move(strips);
    return batch;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_STRIP_TYPES_H_

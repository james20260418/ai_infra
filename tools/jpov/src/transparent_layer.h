// JPOV 透明层契约 —— 任何 WBO 解算结果喂给合成器的统一形态。
//
// 定位（Danis 2026-10-17 定）：雾合成器只认「层」，不认「层是谁生产的」。
// 玻璃 / 广告牌粒子 / 体积火（将来）都各自解算进自己的 WBO 缓冲，然后统一以
// **一个等效层**交给合成器：合成器按代表深度排序做多段（雾 + 层交替）积分。
//
// 一个 WBO 缓冲的三个附件（三处实现同构，见 src/wbo_buffer.h）：
//   accum : RGBA16F，Σ(预乘色×w, α×w) —— 加权平均色 Cavg = accum.rgb / accum.a；
//   reveal: R16F   ，Π(1−α)           —— 该层透射率 T（背景透过率）；
//   depth : R32F   ，Σ(z_view×α×w)    —— 代表深度 z̄ = depth / accum.a（视空间米）。
//
// 由三者导出的**等效层**：
//   C_premult = Cavg · (1 − T)   （预乘色，即本层贡献的亮度）
//   cov       = 1 − T            （遮挡掉多少背景）
//   z̄         = depth / accum.a   （参与排序的代表深度）
//
// ⚠️ 契约边界（**生产者负责，合成器不校验**）：
//   · 每个纹理对每个像素只代表**一个**层；
//   · 各源在沿视线方向上**互不交叠**时合并才是精确的（源内交叠由 WBO 自身近似）。
//   违反时合成器会把整个源丢到 z̄ 的一侧 —— 这是 WBO 的固有边界，不是合并的 bug。

#ifndef JPOV_SRC_TRANSPARENT_LAYER_H_
#define JPOV_SRC_TRANSPARENT_LAYER_H_

#include <glog/logging.h>

namespace jpov {

// 合成器支持的透明层上限（GLSL 里按固定 4 槽展开；空槽不产生深度边界）。
inline constexpr int kMaxTransparentLayers = 4;

// 层的合成语义。
//   kOver     = 遮挡 / 吸收（烟、玻璃）：有预乘色 + 覆盖率，会压暗后方。
//   kAdditive = 发射 / 加法（火焰、激光、火星）：只有发射，**不遮挡**后方（cov 恒为 0）。
//
// 加性层为什么可以塌成一层：加性合成是可交换的（Σ c·α 与顺序无关），
// 内部多少层合并都不丢信息；z̄ 只用于「被前方的遮挡层衰减多少」。
enum class TransparentLayerKind {
    kOver = 0,
    kAdditive = 1,
};

// 一个已解算的透明层（WBO 三附件的只读引用 + 解算分辨率系数）。
struct TransparentLayer {
    unsigned int accum_tex = 0;    // RGBA16F
    unsigned int reveal_tex = 0;   // R16F
    unsigned int depth_tex = 0;    // R32F
    // 解算分辨率 / 主视口分辨率（1、0.5、0.25 …）。合成时按归一化 uv 采样；
    // 低分层的上采样需要深度感知（见 fog_composite_renderer）。
    float scale = 1.0f;
    // 合成语义（见 TransparentLayerKind）。加性层走「不归一化、不遮挡」的路径。
    TransparentLayerKind kind = TransparentLayerKind::kOver;

    bool valid() const {
        return accum_tex != 0 && reveal_tex != 0 && depth_tex != 0;
    }
};

// 本帧要合成的透明层集合（顺序无关：合成器自己按代表深度排序）。
struct TransparentLayerSet {
    TransparentLayer layers[kMaxTransparentLayers];
    int count = 0;

    bool empty() const { return count == 0; }

    // 追加一个层。满时丢弃并告警（不静默截断）。
    // Pre-condition: layer.valid()；scale ∈ (0, 1]。
    void Add(const TransparentLayer& layer) {
        if (!layer.valid()) {
            return;
        }
        if (count >= kMaxTransparentLayers) {
            LOG(WARNING) << "TransparentLayerSet 已达上限 " << kMaxTransparentLayers
                         << "，本帧该层被丢弃";
            return;
        }
        layers[count] = layer;
        ++count;
    }
};

}  // namespace jpov

#endif  // JPOV_SRC_TRANSPARENT_LAYER_H_

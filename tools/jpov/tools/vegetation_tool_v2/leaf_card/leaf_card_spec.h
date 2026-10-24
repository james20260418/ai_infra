// leaf_card/leaf_card_spec.h — 叶卡素材的「生图合同」（生图提示词必须遵守的那几条）。
//
// 这些数字是**跨工具的唯一来源**：
//   * 归一化工具（leaf_card_normalize）按它判定背景/内容、按它做 alpha 软过档；
//   * 查看器 / GLB 烘焙用 `--leaf-black`（= kLeafBlackThreshold）读同一份素材；
//   * 生图提示词把它写成硬性要求（叶子别画成深色、背景必须纯黑）。
//
// 为什么要有这份合同：黑底转 alpha 靠「max(r,g,b) 低于阈值 = 透明」。如果生成的
// 叶子里有接近黑的深色区域，它会被判成透明 —— 叶子自己破洞。约定一个**内容亮度下限**，
// 让「背景可判、内容不会误判」这两件事都有余量。
//
// 判据一律用 **max(r,g,b)**（不是 r+g+b 求和，也不是亮度加权）——
// 与 leaf_texture 的黑底→alpha 判据保持一致；换成别的度量，两边就会对不上。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_SPEC_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_SPEC_H_

namespace jpov {
namespace vegetation {

// 背景：max(r,g,b) <= 这个值 ⇒ 一定透明。
// 生图提示词要求「纯黑背景」，留 16 的余量吸收压缩噪点。
constexpr int kLeafBackgroundMaxChannel = 16;

// 内容：max(r,g,b) >= 这个值 ⇒ 一定不透明。
// 生图提示词要求「叶子颜色避开黑色」（任何通道组合都不能太暗）。
constexpr int kLeafContentMinChannel = 48;

// 黑底→alpha 的软过档中心：区间 [0.5t, 1.5t] 线性过渡，
// 取上面两个常量的中点 ⇒ [16, 48]：背景全透、内容全不透，中间给抗锯齿边缘。
constexpr float kLeafBlackThreshold =
    0.5f * static_cast<float>(kLeafBackgroundMaxChannel + kLeafContentMinChannel);

// 归一化输出的 alpha 裁切（与 leaf_texture 的 kLeafAlphaCutoff 同值，
// 这里重复一份是为了让本工具不依赖 leaf_texture 的 .cc）。
constexpr float kLeafCardAlphaCutoff = 0.5f;

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_SPEC_H_

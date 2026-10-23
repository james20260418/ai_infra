// leaf_card/leaf_card_normalize.h — 把「任意构图的一张叶子图」规整成合规叶卡素材。
//
// 问题：资产格式要求「素材里的根部画在格子指定角/边上，内容撑满宽度」。
// 生图模型做不到（它不知道我们的格子在哪），所以出图之后必须过一道归一化：
//
//   1. 按生图合同（leaf_card_spec.h）切出内容（max(r,g,b) 判据）；
//   2. 找**根部**：叶柄是内容里最细的那一端 —— 用主轴把内容分两半，
//      比较两端的平均「到背景的距离」（距离变换），细的那端是叶柄；
//   3. 求叶柄朝向，**旋转**到「从 anchor 角指向格子内部」的方向
//      （角锚点 = 45° 斜向内，边中点锚点 = 90° 正向内）；
//   4. 按 alpha 做 **AABB 裁剪**，根点正好落在 anchor 角上；
//   5. 输出 RGBA PNG，并把「根点偏差 / 尺寸 / 宽高比 / 灰区占比」报回去。
//
// 为什么旋转角要跟 anchor 绑定：根点在左下角时，叶柄必须朝**右上**（45°），
// 否则叶子会有一半在格子左边被裁掉。这是「四选一 anchor」这套格式的几何后果，
// 不是随便转个 90° 就行的。
//
// 不做的事：不缩放（缩放会改变叶子的实际物理尺寸，该由 width/height 决定）、
// 不改变颜色（除了透出 alpha 边缘的回填）、不做镜像（手性由 leaf type 拆）。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_NORMALIZE_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_NORMALIZE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace jpov {
namespace vegetation {

// 一张 8-bit RGBA 图（行序 = 图片自上而下，与 PNG 一致）。
struct LeafCardImage {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};

struct LeafCardNormalizeOptions {
  // 目标锚点：bottom_left / bottom_center / bottom_right / top_center。
  // 决定叶柄要转到哪个方向、以及最终根点落在哪。
  std::string anchor_mode = "bottom_left";
};

// 归一化的过程数据（全部是给人看的诊断信息）。
struct LeafCardNormalizeReport {
  std::string anchor_mode;

  // 检测到的叶柄方向（度，图像坐标：+x = 0°，y 向下为正角度）。
  double stem_angle_deg = 0.0;
  // 实际施加的旋转（度，正 = 顺时针）。
  double rotation_deg = 0.0;

  // 根点相对目标锚点的偏差（格内归一化，0 = 正好在锚点上）。
  double root_offset_u = 0.0;
  double root_offset_v = 0.0;

  // 输出尺寸与宽高比 —— 宽高比就是该写进 shape 的 `leaf_aspect`（cell_width/cell_height）。
  int width = 0;
  int height = 0;
  double aspect = 0.0;

  // 生图合同体检：落在 [背景上限, 内容下限] 之间的像素占内容的比例。
  // 这些像素会变成半透明边缘；比例过高说明生图没守约定（叶子画太暗/背景不是纯黑）。
  double ambiguous_fraction = 0.0;

  // 为了让根点正好落在锚点上，画布可能比内容的 AABB 小 —— 这里是被裁掉的比例。
  // 明显大于 0 说明「叶子长在了根点的反方向」（例如叶柄在中间、叶片两边都有），
  // 这张图不适合这个 anchor_mode，换一个角或重新出图。
  double dropped_fraction = 0.0;
};

// 归一化。失败返回 false 并写 *error（图里没有内容 / anchor_mode 非法 / 解码失败）。
bool NormalizeLeafCard(const LeafCardImage& input, const LeafCardNormalizeOptions& options,
                       LeafCardImage* output /*output*/,
                       LeafCardNormalizeReport* report /*output*/,
                       std::string* error /*output*/);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_CARD_NORMALIZE_H_

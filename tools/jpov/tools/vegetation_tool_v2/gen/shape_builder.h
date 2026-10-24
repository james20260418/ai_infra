// gen/shape_builder.h — SaplingDump（Blender 原始数据）→ ShapeProto 运行时结构。
//
// 这一步就是原来 export_vegetation_shape.py 里除「调 Blender」之外的全部逻辑，
// 逐条对齐它的语义（含那些看起来奇怪但已成约定的小细节，例如叶卡的 anchor 取
// 「最近**采样点**」而不是最近点）。想改行为请先读那个脚本的注释。
//
// 与 Python 版的已知区别：
//   * 父枝查询用 NearestPointMatcher3d（稀疏栅格），不再是 O(枝数²) 的三重循环；
//     查询语义相同（排除自己那条折线后取最近线段）。
//   * 拓扑序兜底：Python 版把不可达的样条直接追加到末尾，可能造成「子枝排在父枝
//     之前」被 Validate 拒收；这里沿父链把祖先先插进来，保证 parent < id。
//   * 浮点运算在 double 上做（Blender 的 mathutils 是 float32），所以和 Python 产物
//     不是逐字节相同，量级差 ~1e-7 m —— 几何上等价。

#pragma once

#include <string>

#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {
namespace gen {

struct BuildOptions {
  uint32_t per_seg = 8;           // 每段贝塞尔的采样点数
  double leaf_aspect = 0.6;       // 叶卡宽/高
  std::string anchor_mode = "bottom_left";  // type 0 的锚角（镜像 type 自动左右换）
  std::string leaf_type_desc;     // 写进 leaf_types 的自然语言
  std::string species;            // 只用于 description 文案
  // 叶面弯曲（写进每个 leaf type）。全 0 = 平面卡。
  LeafBend leaf_bend;
};

struct BuildStats {
  int branches = 0;
  int cards = 0;
  int mirrored_cards = 0;
};

// 构建 Shape。失败（数据非法、anchor 不在枝上）返回 false 并把原因写进 *error。
bool BuildShape(const SaplingDump& dump, const BuildOptions& options,
                Shape* out /*output*/, BuildStats* stats /*output*/,
                std::string* error /*output*/);

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov

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

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {
namespace gen {

// 枝条噪声（对应 generate.proto 的 ShapeNoise）。
//
// 三个数组的下标都是**枝级**（0 = 主干，1 = 一级侧枝……），与 SaplingTree 的
// `length`/`curve`/`branches` 同一套下标。某级超出数组长度、或写 0 = 该级不启用。
//
// 语义（结果直接烘进折线，shape 资产里不保存这些参数）：
//   * size_jitter：每根枝整体放大/缩小的倍数波动，长度与半径乘同一个因子；
//   * bend_longitudinal：枝中段朝「世界竖直方向在枝法平面上的投影」鼓出去；
//   * bend_lateral：垂直于上者的方向鼓出去（俯视图里的左右扭）。
// 两个弯度都以「枝长的比例」给，且两端位移恒为 0（枝根、枝梢不动）。
struct NoiseOptions {
  std::vector<float> size_jitter;
  std::vector<float> bend_longitudinal;
  std::vector<float> bend_lateral;
  // SaplingTree.seed：同 seed + 同参数 ⇒ 同一棵歪法；换 seed 才换一棵。
  int64_t seed = 0;

  bool Enabled() const {
    for (const std::vector<float>* values : { &size_jitter, &bend_longitudinal, &bend_lateral }) {
      for (float value : *values) {
        if (value != 0.0f) {
          return true;
        }
      }
    }
    return false;
  }
};

struct BuildOptions {
  uint32_t per_seg = 8;           // 每段贝塞尔的采样点数
  double leaf_aspect = 0.6;       // 叶卡宽/高
  std::string anchor_mode = "bottom_left";  // type 0 的锚角（镜像 type 自动左右换）
  std::string leaf_type_desc;     // 写进 leaf_types 的自然语言
  std::string species;            // 只用于 description 文案
  // 叶面弯曲（写进每个 leaf type）。全 0 = 平面卡。
  LeafBend leaf_bend;
  // 枝条不规整。默认全 0 = 关闭 = 与没有这一层时逐字节一致。
  NoiseOptions noise;
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

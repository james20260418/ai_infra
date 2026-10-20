// 扫管：把 Shape（拓扑）变成网格 + 正确的 UV。
//
// 这是整个格式的落地点。关键约束（见 docs/vegetation_asset_format.md §2.3 / §3）：
//   * 逐采样点截面（profile 空 = 正圆），截面沿枝**平行传输**，`tilt` 绝对生效
//   * UV：u = 截面弧长 / 截面周长 ∈ [0,1)；v = 沿枝累计弧长 / 局部 tile
//   * tile = radius × 单位截面周长 ⇒ 一圈正好一张图，且 u/v 用同一尺度（格子是方的）
//   * 子枝入口的 v 从父枝出口接续（不是从 0 开始），否则接口处有一条横向重复线
//
// 单位：米。坐标系：+Y 向上（见 shape.h 的约定）。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_SWEEP_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_SWEEP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {

// 分离式顶点缓冲（与 JPOV 的 MeshData 布局一致：positions/normals 各 3 float/顶点，
// uvs 各 2 float/顶点，indices 为三角形列表）。
struct MeshData {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<uint32_t> indices;

  size_t VertexCount() const { return positions.size() / 3; }
  size_t TriangleCount() const { return indices.size() / 3; }
};

// 扫出所有枝条。不生成端盖：枝条两端要么插在父枝体内、要么是末梢细尖，
// 端盖看不见，加了只是浪费面。
//
// 前置条件：Shape 已通过 Validate()。函数内部不重复做全量校验，只 CHECK 硬前提
// （环顶点数 >= 3、samples >= 2），因为违反这些的资产连循环都跑不起来。
MeshData SweepBranches(const Shape& shape);

// 叶卡：anchor + up + normal + height + 材质给的 width → 四边形。
//
// width_by_type：每个 leaf_type 的实际宽度（米）。由叶材质的 atlas 格子长宽比推导：
//   width = height × (cell_width / cell_height)
// 这里之所以从外部传：**宽度属于材质，不属于拓扑**（拓扑只保证长度）。
// 每个 type 都必须有 > 0 的宽度，缺失视为调用方 bug（CHECK）。
MeshData BuildLeafCards(const Shape& shape, const std::vector<double>& width_by_type);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_SWEEP_H_

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

// 扫管选项。
struct SweepOptions {
  // v 方向的 UV 缩放（= 贴图宽/高）。默认 1.0 = 方形贴图。
  //
  // 为什么需要它：§3 的规则让 u、v 用同一个 tile（一圈一张图 + 纹素方形），
  // 但这只在贴图本身是正方形时成立。非方图（例如 512x1024 的 Bark001）若按
  // 1.0 铺，等于把图硬压进方形：花纹在世界空间被横向拉宽 2×、凸起被压扁，
  // 法线细节也跟着糊。装配方知道材质，把「宽/高」传进来即可。
  //
  // 语义：v' = v × uv_v_scale；等价于把每段的 tile 除以它。
  // 必须 > 0（0 会让 v 恒定，整根枝退化成一条纹素）。
  double uv_v_scale = 1.0;
};

// 分离式顶点缓冲（与 JPOV 的 MeshData 布局一致：positions/normals 各 3 float/顶点，
// uvs 各 2 float/顶点，indices 为三角形列表）。
struct MeshData {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  // 切线（3 float/顶点）：法线贴图 TBN 用。方向 = ∂P/∂u（沿截面周长方向），
  // 解析可得（相邻环顶点差分），不需要从 UV 反推。
  std::vector<float> tangents;
  std::vector<uint32_t> indices;

  size_t VertexCount() const { return positions.size() / 3; }
  size_t TriangleCount() const { return indices.size() / 3; }
};

// 扫出所有枝条。不生成端盖：枝条两端要么插在父枝体内、要么是末梢细尖，
// 端盖看不见，加了只是浪费面。
//
// 前置条件：Shape 已通过 Validate()。函数内部不重复做全量校验，只 CHECK 硬前提
// （环顶点数 >= 3、samples >= 2），因为违反这些的资产连循环都跑不起来。
MeshData SweepBranches(const Shape& shape, const SweepOptions& opts = {});

// 按枝切分（逐枝上色 / 逐枝隐藏用）。返回的每个 MeshData 与 shape.branches 下标一一对应。
//
// 实现依赖 SweepBranches 的顶点布局：每枝占 `samples.size() * (ring+1)` 个顶点、
// 且按 branches 顺序连续排列。两处必须同步修改（同文件内，且有测试钉住）。
std::vector<MeshData> SplitMeshByBranch(const Shape& shape, const MeshData& mesh);

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

// 植被风：从 Shape 拓扑构造**关节表**（烘焙侧，GL-free）。
//
// 关节 = 一段刚性管子的枢轴。引擎（object3d VS）按「子树链」把顶点绕关节枢轴旋转：
//   q_j = q_parent + R(A_parent)·(pivot_j − pivot_parent)
//   A_j = θ_j + (1 − soft_j)·A_parent
//   p'  = q_j + R(A_j)·(p − pivot_j)
// 于是关节的**父子关系、密度、soft** 决定了风吹起来的形状；这份文件是唯一来源。
//
// 三条不变式（`vegetation_glb_joints_test.cc` 逐条断言，改实现必须同步）：
//   1. 关节按枝序生成 ⇒ `parent < id` 恒成立（表天然拓扑序，引擎可单向遍历）；
//      `level == parent.level + 1`、`phase ∈ [0,1)`、`soft ∈ [0,1]`。
//   2. 每枝的保留采样点**升序**、首元素是 0、末元素是最后一个采样点
//      ⇒ 枝首枝尾一定有枢轴，管子不会整体漂移。
//   3. 任一采样点都能被唯一一个关节覆盖（`JointCoveringSample`），且覆盖下标随采样
//      下标**单调不减**（同一个 6 点的位置不会一会儿挂到前一个关节、一会儿挂到后一个）。
//
// 为什么不用 v1 的「Voronoi 标签推断」：网格就是本工具自己扫出来的，顶点属于哪根枝的
// 哪个采样点是**已知**的，直接映射即可；v1 那套跨链三角形（分叉处撕成薄片）的问题
// 从根上不存在。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_JOINTS_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_JOINTS_H_

#include <cstdint>
#include <vector>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/tools/vegetation_tool_v2/bake/bake_options.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {

// 一根枝上的关节（保留的采样点）。
struct BranchJoints {
  std::vector<uint32_t> kept;    // 保留的采样下标（升序；首 = 0，末 = 最后一个采样点）
  std::vector<double> arc;       // 每个保留点的累计弧长（米，与 kept 对齐）
  std::vector<double> cum;       // 所有采样点的累计弧长（米）
  std::vector<double> soft_at;   // 每个保留点的朝向继承度 soft ∈ [0,1]（与 kept 对齐）
};

// 全树的关节布局。
struct JointLayout {
  std::vector<jpov::MeshData::WindJoint> joints;   // 关节表（写进 node extras）
  std::vector<BranchJoints> per_branch;           // 与 shape.branches 对齐
  std::vector<std::vector<int>> joint_of_kept;    // [枝][kept 序号] → 关节下标
};

// 逐关节/逐顶点的相位场：**空间连续**的低频场（与 v1 wind_bake.py 同一条公式）。
// 不连续（哈希 / 逐枝常量）会让相邻顶点位移不一致 ⇒ 网格被撕开。
// 返回 [0,1)。
double SmoothPhase01(double x, double y, double z, double scale);

// 枝上离 p 最近的采样点下标。
// Pre-condition: b.samples 非空（Shape::Validate 保证）。
int FindNearestSampleIndex(const Branch& b, const Vec3d& p);

// 按弧长抽稀一根枝的关节。
//
// 关节数 = clamp(round(枝长 / spacing), 1, max_joints_per_branch) + 1（含首尾），
// 保留点按采样下标**均匀**取（不是按弧长等分 —— 采样点本身已按弧长分布）。
// soft：只有「下垂 + 极细」的枝才 > 0，沿弧长从 0 渐变到 droop/droop_scale。
// Pre-condition: b.samples 非空。
BranchJoints BuildBranchJoints(const Branch& b, const GlbBakeOptions& opts);

// 由整个 shape 建关节表与映射。
//
// Pre-condition: shape.branches 非空；每个 Branch 的 samples 非空；
//   b.parent 要么 -1，要么指向 shape 里存在的枝 id（Shape::Validate 保证）。
JointLayout BuildJointLayout(const Shape& shape, const GlbBakeOptions& opts);

// 覆盖「采样下标 sample_index」的关节下标。
//
// 语义 = 该采样点归属**起点不晚于它的最后一个保留点**（floor by sample index）；
// 同一个采样环上的所有顶点共用它 ⇒ 环不会被切开。
// Pre-condition: joint_of_kept 非空；sample_index 落在该枝的采样范围内。
int JointCoveringSample(const BranchJoints& bj, const std::vector<int>& joint_of_kept,
                        uint32_t sample_index);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_JOINTS_H_

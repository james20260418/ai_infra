// JPOV 软体仿真器 — 「点是否在网格体内」查询（纯 CPU / GL-free / 可单测）
//
// 回答的唯一问题：
//
//     IsPointInMesh(point, mesh)  →  point 是否位于 mesh 所围成的体积内？
//
// 判据用**经典的射线奇偶法（ray casting / even-odd rule）**：从查询点沿一个固定
// 方向射出一条射线，统计它与网格所有三角形的相交次数；奇数次 = 在体内，偶数次
// （含 0 次）= 在体外。这是点-在-多面体判定的教科书算法（Jordan-Brouwer 分离定理
// 的构造性判定）。求交用 Möller–Trumbore 算法（重心坐标形式）。
//
// ============================ 前置条件（务必看清）============================
//   mesh 必须是**封闭曲面（watertight）**——每条边恰好被两个三角形共享，无边界缝、
//   无洞、无自交。奇偶判据只在封闭曲面上成立：单张面片、缺面的壳体会给出无意义的
//   结果。本函数**不做封闭性校验**（那是 O(n) 的拓扑检查，另有归属），调用方须自证。
//
//   边界（表面）点：点恰好落在某个三角形上时，「在体内」本就没有唯一答案
//   （开集与闭集之别），本函数对其返回未定义（实现上由射线穿越数决定），
//   调用方不应依赖。
//
// ============================ 复杂度与后续 ============================
// 本函数是 O(T)（T = 三角形数）的朴素遍历。当前不建任何加速结构——计划稍后在
// 本函数之上为 GLB 建**八叉树**，命中叶节点后再调本函数，故此处不做优化。
//
// 单位/坐标系：与 mesh 完全一致（JPOV 约定长度为米），本函数不做任何换算。
//
// 本文件属软体仿真器包（tools/jpov/soft_mesh_simulator/），但**不依赖仿真器本体**
// （只碰 MeshData 与 geom 向量），因此可被独立单测，也可被将来的八叉树直接复用。
// 按需求者判断，本工具**不放进 geom 库**——它不是通用几何基础设施，而是本线
// 自用的低门槛查询。

#ifndef JPOV_SOFT_MESH_SIMULATOR_POINT_IN_MESH_H_
#define JPOV_SOFT_MESH_SIMULATOR_POINT_IN_MESH_H_

#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace soft_mesh_simulator {

// 判断点 point 是否位于封闭网格 mesh 所围成的体积内（奇偶法，见文件头）。
//
// 参数：
//   point —— 查询点（mesh 所在坐标系，单位米）
//   mesh  —— 封闭网格。索引网格按 indices 三三分组为三角形；非索引网格按
//            「每 3 个顶点一个三角形」解释（与渲染/仿真的 triangle list 语义一致）。
//
// 返回：严格在体内 → true；在体外 → false。表面点未定义（见文件头）。
//
// Pre-condition（不满足即 LOG(FATAL) 崩溃，不带错误数据进下文）：
//   - 网格至少含 1 个完整三角形：
//       索引网格：indices.size() 非 0 且为 3 的倍数；
//       非索引网格：positions.size() 为 3 的倍数且 >= 3；
//   - 索引网格的每个索引都 < positions.size()（越界读是 UB，直接崩）。
//
// 本函数是纯查询：不修改 mesh，且不持有任何状态（可重入、可并发只读调用）。
bool IsPointInMesh(const geom::Vec3<float>& point, const MeshData& mesh);

}  // namespace soft_mesh_simulator
}  // namespace jpov

#endif  // JPOV_SOFT_MESH_SIMULATOR_POINT_IN_MESH_H_

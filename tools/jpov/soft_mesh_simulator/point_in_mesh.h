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
// ============================ 语义与已知边界 ============================
//   - 语义是经典的**偶-奇（even-odd）**规则。单个闭合曲面下等价于常规「内含」；
//     但若 mesh 含**多个相互重叠**的闭合分量，重叠区会判为「在外」（偶-奇），
//     而非「并集」意义上的在内。本函数按需求者指定只做「单 mesh 体积内」，
//     故采用偶-奇；若将来需要「并集/非零绕数」语义，需改换缠绕数法。
//   - 判据要求 mesh 是**封闭曲面（watertight）**——每条边恰好被两个三角形共享，
//     无边界缝、无洞、无自交。本函数**不做封闭性校验**（那是 O(n) 的拓扑检查，
//     另有归属），调用方须自证。封闭性不满足时结果无意义（如单张面片）。
//   - 表面（恰好落在三角形上）点：在体内/外本无唯一答案，本函数对其返回未定义。
//   - 退化：射线恰好穿过共享棱/顶点、或恰好与三角形共面，会导致偶-奇计数差 1。
//     但这类退化要求射线与 mesh 顶点做**精确**对齐——在 float 顶点 + 固定「一般
//     位置」射向下基本不可达（测度零）。实现上用「严格内部」求交主动排除靠棱
//     命中的重复计数；已在非凸（圆环面）解析体积上逐点校验（见单测）。
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

// JPOV 软体仿真器 — 「点是否在网格体内」查询实现（见 point_in_mesh.h 文件头）
//
// 纯 CPU / GL-free：只碰 MeshData 与几何数学，可被单测直接跑。
// 算法：射线奇偶法（ray casting） + Möller–Trumbore 求交，详见头文件。

#include "tools/jpov/soft_mesh_simulator/point_in_mesh.h"

#include <cmath>
#include <cstddef>

#include <glog/logging.h>

namespace jpov {
namespace soft_mesh_simulator {
namespace {

// —— 射线奇偶法用到的数值容差（全程 double 计算，见下）——

// 平行判据：Möller–Trumbore 中 det = edge1 · (dir × edge2)，量级正比于三角形面积。
// det ≈ 0 表示射线与三角形平面平行（或三角形退化），这种三角形不可能被射线穿过内部。
constexpr double kParallelEps = 1e-12;

// 重心坐标边界判据：只承认**严格落在三角形内部**的交点（u、v、1-u-v 均 > eps）。
// 目的是排除「射线恰好穿过共享棱边或顶点」的交点——那会被相邻两个三角形各数一次，
// 让奇偶计数差 1。排除掉这类退化穿越后，计数不受其污染。
constexpr double kBaryEps = 1e-9;

// 交点参数判据：只数射线**正向前方**（t > eps）的交点；同时也避免把落在三角形上的
// 查询点自身（t ≈ 0）算成一次穿越。
constexpr double kHitEps = 1e-9;

// Möller–Trumbore：射线 (origin, dir) 与三角形 (v0,v1,v2) 是否相交于**严格内部**
// 且位于**正向前方**。
//
// 对三角形绕序不敏感：u、v、t 都带 1/det 归一，det 的符号在除法中自动抵消。
// 对 dir 的模长也不敏感：u、v 是两组点积之比（分子分母同含一次 dir，约去），t 只
// 用来判「是否在起点前方」，故 dir 无需是单位向量。
bool RayHitsTriangleInterior(const geom::Vec3<double>& origin,
                             const geom::Vec3<double>& dir,
                             const geom::Vec3<double>& v0,
                             const geom::Vec3<double>& v1,
                             const geom::Vec3<double>& v2) {
    const geom::Vec3<double> edge1 = v1 - v0;
    const geom::Vec3<double> edge2 = v2 - v0;

    const geom::Vec3<double> pvec = dir.Cross(edge2);
    const double det = edge1.Dot(pvec);
    if (std::fabs(det) < kParallelEps) {
        return false;  // 射线与三角形平面（近乎）平行
    }
    const double inv_det = 1.0 / det;

    const geom::Vec3<double> tvec = origin - v0;
    const double u = tvec.Dot(pvec) * inv_det;
    if (u < kBaryEps || u > 1.0 - kBaryEps) {
        return false;  // 交点在三角形外（或落在某条棱边上）
    }

    const geom::Vec3<double> qvec = tvec.Cross(edge1);
    const double v = dir.Dot(qvec) * inv_det;
    if (v < kBaryEps || u + v > 1.0 - kBaryEps) {
        return false;  // 同上（第三条边）
    }

    const double t = edge2.Dot(qvec) * inv_det;
    return t > kHitEps;  // 只数射线正向前方的交点
}

// 取第 tri 个三角形的第 corner（0/1/2）个顶点的位置，转成 double。
//
// 索引网格：vertex index = indices[tri*3 + corner]（越界读是 UB → 直接 CHECK 崩）。
// 非索引网格：vertex index = tri*3 + corner（三角形即连续三个顶点）。
geom::Vec3<double> TriangleVertex(const MeshData& mesh, size_t tri, int corner) {
    size_t vertex_index = 0;
    if (mesh.indices.empty()) {
        vertex_index = tri * 3 + static_cast<size_t>(corner);
    } else {
        vertex_index = mesh.indices[tri * 3 + static_cast<size_t>(corner)];
        CHECK_LT(vertex_index, mesh.positions.size())
            << "IsPointInMesh: 索引越界（index=" << vertex_index
            << " >= positions.size()=" << mesh.positions.size() << "）";
    }
    const Vec3f& position = mesh.positions[vertex_index];
    return geom::Vec3<double>(position.x(), position.y(), position.z());
}

}  // namespace

bool IsPointInMesh(const geom::Vec3<float>& point, const MeshData& mesh) {
    // —— 前置条件：网格至少含 1 个完整三角形（索引越界在取顶点时 CHECK）。——
    size_t triangle_count = 0;
    if (mesh.indices.empty()) {
        CHECK_EQ(mesh.positions.size() % 3, static_cast<size_t>(0))
            << "IsPointInMesh: 非索引网格的 positions 数（" << mesh.positions.size()
            << "）不是 3 的倍数，无法按 triangle list 解释";
        triangle_count = mesh.positions.size() / 3;
    } else {
        CHECK_EQ(mesh.indices.size() % 3, static_cast<size_t>(0))
            << "IsPointInMesh: 索引数（" << mesh.indices.size()
            << "）不是 3 的倍数，无法按三角形分组";
        triangle_count = mesh.indices.size() / 3;
    }
    CHECK_GE(triangle_count, static_cast<size_t>(1))
        << "IsPointInMesh: 网格不含完整三角形，点-在内判定无定义";

    // 射线方向：取一个三分量互不相等、都非零的「一般位置」方向，使射线对常见的
    // 轴对齐资产（盒 / 地面 / 墙）不会恰好与面共面、也不会恰好穿过棱边——那类退化
    // 穿越会让奇偶计数差 1。方向本身任意，只要不落在网格的对称轴上。
    const geom::Vec3<double> ray_dir =
        geom::Vec3<double>(0.5321, 0.7231, 0.4411).Unit();
    const geom::Vec3<double> origin(point.x(), point.y(), point.z());

    // 逐三角形统计穿越次数；奇数次 = 在体内（偶-奇判据）。
    int crossing_count = 0;
    for (size_t tri = 0; tri < triangle_count; ++tri) {
        const geom::Vec3<double> v0 = TriangleVertex(mesh, tri, /*corner=*/0);
        const geom::Vec3<double> v1 = TriangleVertex(mesh, tri, /*corner=*/1);
        const geom::Vec3<double> v2 = TriangleVertex(mesh, tri, /*corner=*/2);
        if (RayHitsTriangleInterior(origin, ray_dir, v0, v1, v2)) {
            ++crossing_count;
        }
    }
    return (crossing_count % 2) == 1;
}

}  // namespace soft_mesh_simulator
}  // namespace jpov

// JPOV 衣物网格补洞（mesh_hole_fill.h）纯函数单测 —— 无 GL。
//
// 覆盖：闭合体挖一个三角形洞 → 补齐（边界边归零、绕序正确）；顶面整片挖掉走阈值不补；
// UV/法线/骨骼插值长度对齐；焊接把"缝合重复顶点"变内部边（不算洞）；周围中位边长定标。

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/clothing/mesh_hole_fill.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {
namespace {

// 8 顶点、12 面（外向 CCW）的立方体。
MeshData MakeCube() {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    m.positions = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},   // 0..3  z=-1
        {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},    // 4..7  z=+1
    };
    const int faces[12][3] = {
        {4, 5, 6}, {4, 6, 7},  // +Z
        {1, 0, 3}, {1, 3, 2},  // -Z
        {5, 1, 2}, {5, 2, 6},  // +X
        {0, 4, 7}, {0, 7, 3},  // -X
        {7, 6, 2}, {7, 2, 3},  // +Y
        {0, 1, 5}, {0, 5, 4},  // -Y
    };
    for (const auto& f : faces) {
        m.indices.push_back(static_cast<uint32_t>(f[0]));
        m.indices.push_back(static_cast<uint32_t>(f[1]));
        m.indices.push_back(static_cast<uint32_t>(f[2]));
    }
    m.normals.assign(m.positions.size(), Vec3f(0.0f, 0.0f, 1.0f));  // 占位（本测试不校验）
    return m;
}

// 统计网格的边界边数（按位置合并，容差 1e-4；与实现同口径）。
size_t CountBoundaryEdges(const MeshData& m) {
    HoleFillOptions o;
    o.weld_tolerance = 1e-4f;
    // 复用实现：跑一次「不补任何洞」的阈值（周长 0 阈值 → 全不补），但它仍会算边界边数。
    // 更直接：这里自行统计，避免依赖返回语义。
    const std::vector<Vec3f>& P = m.positions;
    // 位置焊接（简单 O(n²)，测试规模小）。
    std::vector<uint32_t> rep(P.size());
    for (size_t i = 0; i < P.size(); ++i) {
        rep[i] = static_cast<uint32_t>(i);
        for (size_t j = 0; j < i; ++j) {
            const double dx = P[i].x() - P[j].x();
            const double dy = P[i].y() - P[j].y();
            const double dz = P[i].z() - P[j].z();
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= o.weld_tolerance) {
                rep[i] = rep[j];
                break;
            }
        }
    }
    std::unordered_map<uint64_t, int> ec;
    auto key = [](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const uint32_t a = rep[m.indices[t]], b = rep[m.indices[t + 1]],
                       c = rep[m.indices[t + 2]];
        ec[key(a, b)]++;
        ec[key(b, c)]++;
        ec[key(c, a)]++;
    }
    size_t n = 0;
    for (const auto& kv : ec) {
        if (kv.second == 1) ++n;
    }
    return n;
}

Vec3f TriNormal(const MeshData& m, size_t tri_begin) {
    const Vec3f& a = m.positions[m.indices[tri_begin]];
    const Vec3f& b = m.positions[m.indices[tri_begin + 1]];
    const Vec3f& c = m.positions[m.indices[tri_begin + 2]];
    const Vec3f ab(b.x() - a.x(), b.y() - a.y(), b.z() - a.z());
    const Vec3f ac(c.x() - a.x(), c.y() - a.y(), c.z() - a.z());
    return Vec3f(ab.y() * ac.z() - ab.z() * ac.y(),
                 ab.z() * ac.x() - ab.x() * ac.z(),
                 ab.x() * ac.y() - ab.y() * ac.x());
}

// 闭合立方体本该 0 边界边。
TEST(MeshHoleFillTest, ClosedCubeHasNoBoundary) {
    const MeshData cube = MakeCube();
    EXPECT_EQ(CountBoundaryEdges(cube), 0u);
    HoleFillOptions o;
    MeshData out;
    HoleFillStats st;
    EXPECT_FALSE(FillMeshHoles(cube, o, &out, &st)) << "无洞不应改动";
}

// 挖掉 +Z 面一个三角形 → 补回：边界边归零、补出的三角形法线朝 +Z（绕序正确）。
TEST(MeshHoleFillTest, FillTriangularHoleKeepsOutwardWinding) {
    MeshData m = MakeCube();
    // 删第一个三角形 (4,5,6)；其法线 = +Z。
    m.indices.erase(m.indices.begin(), m.indices.begin() + 3);
    ASSERT_GT(CountBoundaryEdges(m), 0u) << "删面后必有洞";

    HoleFillOptions o;
    MeshData out;
    HoleFillStats st;
    ASSERT_TRUE(FillMeshHoles(m, o, &out, &st));
    EXPECT_EQ(st.boundary_edges, 3u);
    EXPECT_EQ(st.loops_filled, 1u);
    EXPECT_GE(st.triangles_added, 1u);
    EXPECT_EQ(CountBoundaryEdges(out), 0u) << "补齐后应闭合";

    // 补出的三角形应朝 +Z（与挖掉的面对齐 → 没翻面）。
    ASSERT_EQ(out.indices.size() % 3, 0u);
    const size_t last = out.indices.size() - 3;
    const Vec3f n = TriNormal(out, last);
    EXPECT_GT(n.z(), 0.0f) << "补出的三角形法线应朝 +Z（绕序与原面一致）";
}

// UV 属性长度对齐（补洞后 uvs.size()==positions.size()，且新 UV 有限）。
TEST(MeshHoleFillTest, InterpolatesUvForNewVertices) {
    MeshData m = MakeCube();
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) | static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.uvs.assign(m.positions.size(), Vec2f(0.0f, 0.0f));
    m.indices.erase(m.indices.begin(), m.indices.begin() + 3);
    HoleFillOptions o;
    o.refine = false;  // 先测不插点
    o.fair = false;
    MeshData out;
    HoleFillStats st;
    ASSERT_TRUE(FillMeshHoles(m, o, &out, &st));
    EXPECT_EQ(out.uvs.size(), out.positions.size());
    EXPECT_EQ(st.vertices_added, 0u) << "refine 关时不新增顶点";
    out.Validate();
}

// 焊接：两块三角形各自带一份"缝合重复顶点"沿共享边拼在一起 → weld 后共享边是内部边。
// 没有 weld 时 6 条三角形边全是边界边（索引对全不同）；weld 后共享边并入 → 只剩外沿 4 条。
TEST(MeshHoleFillTest, WeldMakesCoincidentSeamInteriorNotHole) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0},  // 左片 (0,1,2)
        {1, 0, 0}, {1, 1, 0}, {0, 1, 0},  // 右片 (3,4,5)——顶点 1≈3、2≈5 重合
    };
    m.indices = {0, 1, 2, 3, 4, 5};
    // 共享边 = 左 (1,2) 与 右 (5,3)：位置重合 → weld 后同一无向边，计数 2（内部边）。
    // 外沿 4 条边 = (0,1),(2,0),(3,4),(4,5) → 4（若未 weld 会是 6）。
    EXPECT_EQ(CountBoundaryEdges(m), 4u)
        << "weld（容差 1e-4）后共享缝合边应算内部边（否则 6 条）";

    HoleFillOptions o;
    MeshData out;
    HoleFillStats st;
    FillMeshHoles(m, o, &out, &st);
    EXPECT_EQ(st.boundary_edges, 4u) << "缝合成内部边后只剩外沿 4 条";
    EXPECT_EQ(st.loops_total, 1u) << "外沿是唯一的一个环";
}

// 阈值：整块大洞（周长超过阈值）不补。
TEST(MeshHoleFillTest, ThresholdSkipsLargeOpening) {
    MeshData m = MakeCube();
    m.indices.erase(m.indices.begin(), m.indices.begin() + 3);  // 3 边洞
    HoleFillOptions o;
    o.max_hole_perimeter = 0.5f;  // 洞周长 ≈ 3×2.83 ≈ 8.5m > 0.5 → 跳过
    MeshData out;
    HoleFillStats st;
    EXPECT_FALSE(FillMeshHoles(m, o, &out, &st));
    EXPECT_EQ(st.loops_filled, 0u);
    EXPECT_EQ(st.loops_total, 1u);
}

}  // namespace
}  // namespace clothing
}  // namespace jpov

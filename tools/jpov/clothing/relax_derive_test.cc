// JPOV 穿衣工具 — 由固定点推导松弛度（relax_derive）纯函数单测
//
// 覆盖：固定点恒为 0、测地距离生长（近小远大 / 等距相等）、显式半径饱和、自动定标、
// 焊接缝合（缝两侧松弛度一致）、不连通分量置 1、Laplacian 平滑（锚点冻结 / 降 Dirichlet 能量 /
// 保持 [0,1]）。全部构造性小网格，无 GL / glTF。

#include "tools/jpov/clothing/relax_derive.h"

#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {
namespace {

// 造一个 nx×ny 的平面网格（y=0；x=i*dx、z=j*dx）；顶点下标 = j*nx + i。
// 每格两个三角形。索引网格。
MeshData MakeGridMesh(int nx, int ny, float dx) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            m.positions.push_back(
                Vec3f(static_cast<float>(i) * dx, 0.0f, static_cast<float>(j) * dx));
        }
    }
    const auto vid = [nx](int i, int j) -> uint32_t {
        return static_cast<uint32_t>(j * nx + i);
    };
    for (int j = 0; j + 1 < ny; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            const uint32_t a = vid(i, j);
            const uint32_t b = vid(i + 1, j);
            const uint32_t c = vid(i + 1, j + 1);
            const uint32_t d = vid(i, j + 1);
            m.indices.push_back(a);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(a);
            m.indices.push_back(c);
            m.indices.push_back(d);
        }
    }
    return m;
}

// 沿 X 的一串顶点 x=0,1,2,3,4，三角形 (0,1,2)(2,3,4)（连通链）。
MeshData MakeChainMesh() {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    for (int i = 0; i < 5; ++i) {
        m.positions.push_back(Vec3f(static_cast<float>(i), 0.0f, 0.0f));
    }
    m.indices = {0, 1, 2, 2, 3, 4};
    return m;
}

// Dirichlet 能量：沿网格每条无向边 (u,v)，Σ (r_u − r_v)²。用于验证平滑降低不平滑度。
double DirichletEnergy(const MeshData& m, const std::vector<float>& r) {
    std::set<std::pair<uint32_t, uint32_t>> edges;
    const auto add = [&edges](uint32_t a, uint32_t b) {
        if (a == b) {
            return;
        }
        edges.insert(std::minmax(a, b));
    };
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        add(m.indices[i], m.indices[i + 1]);
        add(m.indices[i + 1], m.indices[i + 2]);
        add(m.indices[i + 2], m.indices[i]);
    }
    double e = 0.0;
    for (const std::pair<uint32_t, uint32_t>& p : edges) {
        const double d = static_cast<double>(r[p.first]) - r[p.second];
        e += d * d;
    }
    return e;
}

std::vector<uint32_t> Anchors(std::initializer_list<uint32_t> a) {
    return std::vector<uint32_t>(a);
}

// ── 固定点恒为 0；测地距离生长（近小远大、等距相等）──
TEST(RelaxDeriveTest, GrowsWithGeodesicDistance) {
    const MeshData mesh = MakeGridMesh(/*nx=*/5, /*ny=*/5, /*dx=*/1.0f);
    const std::vector<float> r = DeriveLagRatioFromAnchors(
        mesh, Anchors({0}), /*weld=*/0.0f, /*radius=*/0.0f, /*smooth=*/0, nullptr);
    ASSERT_EQ(r.size(), mesh.positions.size());
    // 固定点 = 0。
    EXPECT_FLOAT_EQ(r[0], 0.0f);
    // 全部落在 [0,1]。
    for (float v : r) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
    // 网格无对角线边 ⇒ 测地距离 = 曼哈顿距离。自动定标（最远 = (4,4)，距离 8 → 1.0）。
    EXPECT_NEAR(r[12], 0.5f, 1e-4f);  // (i=2,j=2) 距离 4
    EXPECT_NEAR(r[24], 1.0f, 1e-4f);  // (i=4,j=4) 距离 8（最远）
    // 近 < 远。
    EXPECT_LT(r[12], r[24]);
    EXPECT_LT(r[0], r[12]);
    // 等距顶点相等（(0,2)=下标 10、(2,0)=下标 2 都距 0 为 2）。
    EXPECT_NEAR(r[10], r[2], 1e-4f);
}

// ── 多个固定点都恒为 0；其余 > 0 ──
TEST(RelaxDeriveTest, AllAnchorsAreZero) {
    const MeshData mesh = MakeGridMesh(5, 5, 1.0f);
    const std::vector<float> r = DeriveLagRatioFromAnchors(mesh, Anchors({0, 24}),
                                                           0.0f, 0.0f, 0, nullptr);
    EXPECT_FLOAT_EQ(r[0], 0.0f);
    EXPECT_FLOAT_EQ(r[24], 0.0f);
    EXPECT_GT(r[12], 0.0f);  // 两个固定点之间必然 > 0
}

// ── 显式半径：测地距离 ≥ 半径饱和到 1.0，其余线性 ──
TEST(RelaxDeriveTest, ExplicitRadiusSaturates) {
    const MeshData mesh = MakeChainMesh();
    const std::vector<float> r = DeriveLagRatioFromAnchors(mesh, Anchors({0}),
                                                           /*weld=*/0.0f,
                                                           /*radius=*/2.0f,
                                                           /*smooth=*/0, nullptr);
    EXPECT_FLOAT_EQ(r[0], 0.0f);
    EXPECT_NEAR(r[1], 0.5f, 1e-4f);  // 距离 1 → 0.5
    EXPECT_NEAR(r[2], 1.0f, 1e-4f);  // 距离 2 → 1.0（饱和）
    EXPECT_NEAR(r[3], 1.0f, 1e-4f);  // 距离 3 → 1.0
    EXPECT_NEAR(r[4], 1.0f, 1e-4f);  // 距离 4 → 1.0
}

// ── 不与固定点连通的分量 → 1.0（没有东西约束它）──
TEST(RelaxDeriveTest, DisconnectedComponentIsFree) {
    MeshData mesh;
    mesh.flags = MeshVertexFlags::kPosition;
    mesh.positions = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1),
                      Vec3f(5, 0, 0), Vec3f(6, 0, 0), Vec3f(5, 0, 1)};
    mesh.indices = {0, 1, 2, /*另一连通分量：*/ 3, 4, 5};
    const std::vector<float> r = DeriveLagRatioFromAnchors(mesh, Anchors({0}), 0.0f,
                                                           0.0f, 0, nullptr);
    EXPECT_FLOAT_EQ(r[0], 0.0f);
    EXPECT_FLOAT_EQ(r[3], 1.0f);  // 另一分量：全自由
    EXPECT_FLOAT_EQ(r[4], 1.0f);
    EXPECT_FLOAT_EQ(r[5], 1.0f);
}

// ── 焊接：缝合缝两侧（位置重合的重复顶点）得到**一致**松弛度 ──
TEST(RelaxDeriveTest, WeldSeamSharesRelax) {
    // 一个四边形，公共边被拆成重复顶点：
    //   三角形 A：(0,1,2) = (0,0,0)(1,0,0)(0,0,1)
    //   三角形 B：(3,4,5) = (1,0,0)(0,0,1)(1,0,1)   ← 1≡3、2≡4 位置重合
    MeshData mesh;
    mesh.flags = MeshVertexFlags::kPosition;
    mesh.positions = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1),
                      Vec3f(1, 0, 0), Vec3f(0, 0, 1), Vec3f(1, 0, 1)};
    mesh.indices = {0, 1, 2, 3, 4, 5};

    // 焊接（容差 1mm）：缝两侧合并 → 5 号顶点可达（松弛度 < 1）。
    const std::vector<float> welded =
        DeriveLagRatioFromAnchors(mesh, Anchors({0}), /*weld=*/1e-3f,
                                  /*radius=*/0.0f, /*smooth=*/0, nullptr);
    EXPECT_FLOAT_EQ(welded[0], 0.0f);
    EXPECT_NEAR(welded[1], welded[3], 1e-6f);  // 缝两侧一致
    EXPECT_NEAR(welded[2], welded[4], 1e-6f);
    EXPECT_LT(welded[3], 1.0f);  // 焊接后可达（非全自由）

    // 不焊接：三角形 B 与锚点不连通 → 全 1.0。
    const std::vector<float> raw = DeriveLagRatioFromAnchors(
        mesh, Anchors({0}), /*weld=*/0.0f, /*radius=*/0.0f, /*smooth=*/0, nullptr);
    EXPECT_FLOAT_EQ(raw[0], 0.0f);
    EXPECT_FLOAT_EQ(raw[3], 1.0f);
    EXPECT_FLOAT_EQ(raw[4], 1.0f);
    EXPECT_FLOAT_EQ(raw[5], 1.0f);
}

// ── Laplacian 平滑：锚点仍为 0、保持 [0,1]、降低 Dirichlet 能量（更平滑）──
TEST(RelaxDeriveTest, SmoothingPinsAnchorsAndReducesEnergy) {
    const MeshData mesh = MakeGridMesh(5, 5, 1.0f);
    const std::vector<float> raw = DeriveLagRatioFromAnchors(mesh, Anchors({0}), 0.0f,
                                                             0.0f, /*smooth=*/0, nullptr);
    RelaxDeriveStats stats;
    const std::vector<float> sm = DeriveLagRatioFromAnchors(
        mesh, Anchors({0}), 0.0f, 0.0f, /*smooth=*/40, &stats);
    EXPECT_EQ(stats.smoothing_passes, 40u);
    EXPECT_FLOAT_EQ(sm[0], 0.0f);  // 锚点冻结
    for (float v : sm) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
    // 平滑后总不平滑度（Dirichlet 能量）应下降。
    EXPECT_LT(DirichletEnergy(mesh, sm), DirichletEnergy(mesh, raw));
    // 最远点仍应比中点大。
    EXPECT_GT(sm[24], sm[12]);
}

// ── 统计字段 ──
TEST(RelaxDeriveTest, StatsReported) {
    const MeshData mesh = MakeGridMesh(5, 5, 1.0f);
    RelaxDeriveStats stats;
    const std::vector<float> r = DeriveLagRatioFromAnchors(
        mesh, Anchors({0, 24}), 0.0f, 0.0f, 0, &stats);
    EXPECT_EQ(r.size(), 25u);
    EXPECT_EQ(stats.vertex_count, 25u);
    EXPECT_EQ(stats.anchor_count, 2u);
    EXPECT_EQ(stats.reachable_count, 25u);
    EXPECT_EQ(stats.unreachable_count, 0u);
    EXPECT_GT(stats.max_geodesic_m, 0.0f);
}

// ── 注入 mesh：设 kRelax + relaxations；顶点数不匹配则不动 ──
TEST(RelaxDeriveTest, ApplyRelaxToMesh) {
    MeshData mesh;
    mesh.flags = MeshVertexFlags::kPosition;
    mesh.positions = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1)};
    const std::vector<float> lag = {0.0f, 0.5f, 1.0f};
    EXPECT_TRUE(ApplyRelaxToMesh(lag, /*max_lag=*/0.05f, &mesh));
    EXPECT_TRUE(MeshHasFlag(mesh.flags, MeshVertexFlags::kRelax));
    ASSERT_EQ(mesh.relaxations.size(), 3u);
    EXPECT_FLOAT_EQ(mesh.relaxations[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(mesh.relaxations[1].x(), 0.5f);
    EXPECT_FLOAT_EQ(mesh.relaxations[2].x(), 1.0f);
    for (const Vec2f& r : mesh.relaxations) {
        EXPECT_FLOAT_EQ(r.y(), 0.05f);
    }
    mesh.Validate();  // 声明与数据一致（不崩）

    // 越界 lag_ratio 会被 clamp。
    const std::vector<float> out_of_range = {-1.0f, 2.0f, 0.3f};
    EXPECT_TRUE(ApplyRelaxToMesh(out_of_range, 0.1f, &mesh));
    EXPECT_FLOAT_EQ(mesh.relaxations[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(mesh.relaxations[1].x(), 1.0f);

    // 顶点数不匹配：返回 false 且不改 mesh。
    MeshData other;
    other.flags = MeshVertexFlags::kPosition;
    other.positions = {Vec3f(0, 0, 0), Vec3f(1, 0, 0)};
    EXPECT_FALSE(ApplyRelaxToMesh(lag, 0.05f, &other));
    EXPECT_FALSE(MeshHasFlag(other.flags, MeshVertexFlags::kRelax));
    EXPECT_TRUE(other.relaxations.empty());
}

}  // namespace
}  // namespace clothing
}  // namespace jpov

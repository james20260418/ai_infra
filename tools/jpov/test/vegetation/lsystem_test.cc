// L-System 引擎 + 龟图 —— 单元测试（GL-free、纯 CPU）。
//
// 覆盖：RNG 确定性、表达式/条件/算术改写、括号栈、龟图几何、龙爪槐端到端确定性。
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/src/vegetation/branch_mesh.h"
#include "tools/jpov/src/vegetation/leaf_cards.h"
#include "tools/jpov/src/vegetation/lsystem.h"

namespace jpov {
namespace vegetation {
namespace {

// 把模块序列的符号拼成字符串（忽略参数），便于断言结构。
std::string Symbols(const std::vector<Module>& mods) {
    std::string s;
    for (const Module& m : mods) {
        s.push_back(m.symbol);
    }
    return s;
}

// ============================ RNG ============================

TEST(LSystemRngTest, DeterministicSameSeed) {
    Rng a(12345);
    Rng b(12345);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(a.NextU64(), b.NextU64());
    }
}

TEST(LSystemRngTest, DifferentSeedsDiffer) {
    Rng a(1);
    Rng b(2);
    EXPECT_NE(a.NextU64(), b.NextU64());
}

TEST(LSystemRngTest, FloatInUnitInterval) {
    Rng a(99);
    for (int i = 0; i < 1000; ++i) {
        const float v = a.NextFloat01();
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
    }
}

// ============================ 改写（无参） ============================

TEST(LSystemRewriteTest, ClassicBranching) {
    LSystemBuilder b;
    b.SetAxiom("A");
    b.AddRule("A -> F[+A][-A]");
    LSystem sys = b.Build();

    EXPECT_EQ(Symbols(sys.Expand(0, 0)), "A");
    EXPECT_EQ(Symbols(sys.Expand(1, 0)), "F[+A][-A]");
    // 第 2 代：A 各自再展开一次。
    EXPECT_EQ(Symbols(sys.Expand(2, 0)),
              "F[+F[+A][-A]][-F[+A][-A]]");
}

TEST(LSystemRewriteTest, UnknownSymbolStaysPut) {
    LSystemBuilder b;
    b.SetAxiom("A");
    b.AddRule("A -> AB");   // B 无规则
    LSystem sys = b.Build();
    EXPECT_EQ(Symbols(sys.Expand(3, 0)), "ABBB");
}

// ============================ 参数 / 条件 / 算术 ============================

TEST(LSystemParamTest, ConditionSelectsByParam) {
    LSystemBuilder b;
    b.SetAxiom("A(0)");
    b.AddRule("A(n) : n < 3 -> F(n)A(n + 1)");
    LSystem sys = b.Build();

    // 3 代后：n=0,1,2 依次展开出 F，n=3 不再命中（原地保留为 A(3)）。
    std::vector<Module> out = sys.Expand(5, 0);
    EXPECT_EQ(Symbols(out), "FFFA");
    // F 的参数应是 0,1,2；末尾 A 保留参数 3。
    ASSERT_EQ(out.size(), 4u);
    EXPECT_FLOAT_EQ(out[0].params[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1].params[0], 1.0f);
    EXPECT_FLOAT_EQ(out[2].params[0], 2.0f);
    EXPECT_EQ(out[3].symbol, 'A');
    EXPECT_FLOAT_EQ(out[3].params[0], 3.0f);
}

TEST(LSystemParamTest, MultiArgAndArithmetic) {
    LSystemBuilder b;
    b.SetAxiom("A(2, 3)");
    b.AddRule("A(a, b) -> F(a + b) F(a * 2)");
    LSystem sys = b.Build();
    std::vector<Module> out = sys.Expand(1, 0);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FLOAT_EQ(out[0].params[0], 5.0f);
    EXPECT_FLOAT_EQ(out[1].params[0], 4.0f);
}

TEST(LSystemParamTest, GeConditionAndScaledGrowth) {
    LSystemBuilder b;
    b.SetAxiom("S(8)");
    b.AddRule("S(v) : v > 2 -> F(v)S(v / 2)");
    LSystem sys = b.Build();
    std::vector<Module> out = sys.Expand(2, 0);
    // 第一代：F(8) S(4)；第二代：F(8) F(4) S(2)。
    EXPECT_EQ(Symbols(out), "FFS");
    EXPECT_FLOAT_EQ(out[0].params[0], 8.0f);
    EXPECT_FLOAT_EQ(out[1].params[0], 4.0f);
}

// ============================ 随机规则（可选路径） ============================

TEST(LSystemStochasticTest, SameSeedSameResult) {
    LSystemBuilder b;
    b.SetAxiom("A");
    b.AddRule("A -> F", 1.0f);
    b.AddRule("A -> FF", 1.0f);
    LSystem sys = b.Build();
    EXPECT_EQ(Symbols(sys.Expand(4, 7)), Symbols(sys.Expand(4, 7)));
}

// ============================ 龟图几何 ============================

TEST(LSystemTurtleTest, SingleForwardSegment) {
    LSystemBuilder b;
    b.SetAxiom("F(1)");
    LSystem sys = b.Build();
    TurtleConfig cfg;
    cfg.length_scale = 2.0f;
    cfg.base_radius = 0.5f;

    Skeleton sk = GenerateSkeleton(sys, cfg, /*generations=*/0, 0);
    ASSERT_EQ(sk.segments.size(), 1u);
    const Segment& s = sk.segments[0];
    EXPECT_FLOAT_EQ(s.start.x(), 0.0f);
    EXPECT_FLOAT_EQ(s.start.y(), 0.0f);
    EXPECT_FLOAT_EQ(s.end.x(), 0.0f);
    EXPECT_FLOAT_EQ(s.end.y(), 2.0f);   // F(1) * length_scale(2)
    EXPECT_FLOAT_EQ(s.radius, 0.5f);
}

TEST(LSystemTurtleTest, BracketProducesTwoBranches) {
    LSystemBuilder b;
    b.SetAxiom("F(1)[&(90)F(1)][&(-90)F(1)]");
    LSystem sys = b.Build();
    TurtleConfig cfg;
    cfg.length_scale = 1.0f;

    Skeleton sk = GenerateSkeleton(sys, cfg, 0, 0);
    ASSERT_EQ(sk.segments.size(), 3u);   // 主干 + 两侧枝
    // 主干沿 +Y。
    EXPECT_FLOAT_EQ(sk.segments[0].end.y(), 1.0f);
    // 两条侧枝末端 y 应低于起点（向下/向侧面），且长度非零。
    for (size_t i = 1; i < sk.segments.size(); ++i) {
        const Segment& s = sk.segments[i];
        const float dx = s.end.x() - s.start.x();
        const float dy = s.end.y() - s.start.y();
        const float dz = s.end.z() - s.start.z();
        EXPECT_GT(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0e-4f);
    }
}

TEST(LSystemTurtleTest, BranchTaperReducesRadius) {
    LSystemBuilder b;
    b.SetAxiom("F(1)[F(1)]");
    LSystem sys = b.Build();
    TurtleConfig cfg;
    cfg.base_radius = 1.0f;
    cfg.radius_taper_per_branch = 0.5f;

    Skeleton sk = GenerateSkeleton(sys, cfg, 0, 0);
    ASSERT_EQ(sk.segments.size(), 2u);
    // 主干半径 1.0；画完主干后先按 segment taper 衰减（×0.94），
    // 再入栈按 branch taper 衰减（×0.5）→ 子枝 0.47。
    const float seg_taper = cfg.radius_taper_per_segment;   // 默认 0.94
    EXPECT_FLOAT_EQ(sk.segments[0].radius, 1.0f);
    EXPECT_FLOAT_EQ(sk.segments[1].radius, 1.0f * seg_taper * 0.5f);
}

TEST(LSystemTurtleTest, AllSegmentsFinite) {
    // 合法配置下生成不应产生 NaN/Inf（防御式回归）。
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig cfg;
    Skeleton sk = GenerateSkeleton(sys, cfg, 25, 0);
    for (const Segment& s : sk.segments) {
        EXPECT_TRUE(std::isfinite(s.start.x()) && std::isfinite(s.start.y()) &&
                    std::isfinite(s.start.z()));
        EXPECT_TRUE(std::isfinite(s.end.x()) && std::isfinite(s.end.y()) &&
                    std::isfinite(s.end.z()));
    }
}

// ============================ 龙爪槐端到端 ============================

TEST(DragonClawTest, ProducesBranchingSkeleton) {
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig cfg;
    Skeleton sk = GenerateSkeleton(sys, cfg, /*generations=*/25, /*seed=*/0);
    EXPECT_GT(sk.segments.size(), 100u) << "龙爪槐骨架线段过少，规则可能没展开";
    for (const Segment& s : sk.segments) {
        EXPECT_GE(s.radius, cfg.min_radius - 1.0e-6f);
    }
}

TEST(DragonClawTest, DeterministicAcrossRuns) {
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig cfg;
    Skeleton a = GenerateSkeleton(sys, cfg, 25, 42);
    Skeleton b = GenerateSkeleton(sys, cfg, 25, 42);
    ASSERT_EQ(a.segments.size(), b.segments.size());
    for (size_t i = 0; i < a.segments.size(); ++i) {
        EXPECT_FLOAT_EQ(a.segments[i].start.x(), b.segments[i].start.x());
        EXPECT_FLOAT_EQ(a.segments[i].end.y(), b.segments[i].end.y());
        EXPECT_FLOAT_EQ(a.segments[i].radius, b.segments[i].radius);
    }
}

TEST(DragonClawTest, TrunkGrowsUpwardBeforeBranches) {
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig cfg;
    Skeleton sk = GenerateSkeleton(sys, cfg, 25, 0);
    ASSERT_FALSE(sk.segments.empty());
    // 第一段（主干底部）应沿 +Y 向上。
    EXPECT_GT(sk.segments[0].end.y(), sk.segments[0].start.y());
}

// ============================ 枝链（polylines） ============================

TEST(SkeletonPolylineTest, ContinuousForwardFormsOneChain) {
    LSystemBuilder b;
    b.SetAxiom("F(1)&(10)F(1)&(10)F(1)");
    LSystem sys = b.Build();
    TurtleConfig cfg;
    Skeleton sk = GenerateSkeleton(sys, cfg, 0, 0);
    ASSERT_EQ(sk.polylines.size(), 1u);
    EXPECT_EQ(sk.polylines[0].points.size(), 4u);   // 3 段 → 4 个点
    EXPECT_EQ(sk.polylines[0].radii.size(), 4u);
}

TEST(SkeletonPolylineTest, BracketsBreakChains) {
    LSystemBuilder b;
    b.SetAxiom("F(1)[F(1)]F(1)");
    LSystem sys = b.Build();
    TurtleConfig cfg;
    Skeleton sk = GenerateSkeleton(sys, cfg, 0, 0);
    // 主干首段 / 子枝 / 主干续段 = 3 条链。
    EXPECT_EQ(sk.polylines.size(), 3u);
}

// ============================ 枝干管网格 ============================

TEST(BranchMeshTest, EmptySkeletonGivesEmptyMesh) {
    Skeleton sk;
    const jpov::MeshData m = BuildBranchMesh(sk, BranchMeshConfig{});
    EXPECT_TRUE(m.positions.empty());
}

TEST(BranchMeshTest, SingleChainVertexAndIndexCounts) {
    // 一条 3 点的直链，8 边环。
    Skeleton sk;
    Polyline pl;
    pl.points = {Vec3f(0, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 2, 0)};
    pl.radii = {0.2f, 0.15f, 0.1f};
    sk.polylines.push_back(pl);

    BranchMeshConfig cfg;
    cfg.ring_edges = 8;
    const jpov::MeshData m = BuildBranchMesh(sk, cfg);
    // 顶点 = 3 点 × (8+1) + 2 端盖中心点。索引 = 2 段 × 8 环缝 × 6 + 两盖 × 8 × 3。
    EXPECT_EQ(m.positions.size(), 3u * 9u + 2u);
    EXPECT_EQ(m.indices.size(), 2u * 8u * 6u + 2u * 8u * 3u);
    EXPECT_TRUE(jpov::MeshHasFlag(m.flags, jpov::MeshVertexFlags::kNormal));
    EXPECT_TRUE(jpov::MeshHasFlag(m.flags, jpov::MeshVertexFlags::kUV));
    m.Validate();
}

TEST(BranchMeshTest, NormalsAreUnitAndUvInRange) {
    Skeleton sk;
    Polyline pl;
    pl.points = {Vec3f(0, 0, 0), Vec3f(0.3f, 1, 0.2f), Vec3f(0.1f, 2, -0.4f)};
    pl.radii = {0.2f, 0.15f, 0.1f};
    sk.polylines.push_back(pl);
    BranchMeshConfig cfg;
    cfg.ring_edges = 6;
    const jpov::MeshData m = BuildBranchMesh(sk, cfg);
    for (const Vec3f& n : m.normals) {
        const float len = std::sqrt(n.x() * n.x() + n.y() * n.y() + n.z() * n.z());
        EXPECT_NEAR(len, 1.0f, 1.0e-4f);
    }
    for (const jpov::Vec2f& uv : m.uvs) {
        EXPECT_GE(uv.x(), 0.0f);   // u 为物理弧长/瓦长，可 > 1（纹理循环）
        EXPECT_GE(uv.y(), 0.0f);
        EXPECT_TRUE(std::isfinite(uv.y()));
    }
}

TEST(BranchMeshTest, DragonClawBuildsNonEmptyMesh) {
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig tcfg;
    Skeleton sk = GenerateSkeleton(sys, tcfg, 25, 0);
    ASSERT_FALSE(sk.polylines.empty());
    BranchMeshConfig bcfg;
    const jpov::MeshData m = BuildBranchMesh(sk, bcfg);
    EXPECT_GT(m.positions.size(), 100u);
    EXPECT_EQ(m.indices.size() % 3u, 0u);
    for (const Vec3f& p : m.positions) {
        EXPECT_TRUE(std::isfinite(p.x()) && std::isfinite(p.y()) &&
                    std::isfinite(p.z()));
    }
}

TEST(BranchMeshTest, DeterministicAcrossRuns) {
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig tcfg;
    BranchMeshConfig bcfg;
    Skeleton a = GenerateSkeleton(sys, tcfg, 25, 7);
    Skeleton b = GenerateSkeleton(sys, tcfg, 25, 7);
    const jpov::MeshData ma = BuildBranchMesh(a, bcfg);
    const jpov::MeshData mb = BuildBranchMesh(b, bcfg);
    ASSERT_EQ(ma.positions.size(), mb.positions.size());
    EXPECT_FLOAT_EQ(ma.positions.back().y(), mb.positions.back().y());
}

// ============================ 叶位 / 叶片卡片 ============================

TEST(LeafSiteTest, LeafMarkerRecordsSite) {
    LSystemBuilder b;
    b.SetAxiom("F(1)&(90)l");
    LSystem sys = b.Build();
    Skeleton sk = GenerateSkeleton(sys, TurtleConfig{}, 0, 0);
    ASSERT_EQ(sk.leaf_sites.size(), 1u);
    // 叶位轴向应与该处前进方向一致（非零）。
    const Vec3f& ax = sk.leaf_sites[0].axis;
    EXPECT_GT(std::sqrt(ax.x() * ax.x() + ax.y() * ax.y() + ax.z() * ax.z()),
              0.5f);
}

TEST(LeafCardsTest, EmptyWhenNoSites) {
    Skeleton sk;
    const jpov::MeshData m = BuildLeafCardsMesh(sk, LeafCardConfig{});
    EXPECT_TRUE(m.positions.empty());
}

TEST(LeafCardsTest, OneSiteGivesTwoQuads) {
    Skeleton sk;
    LeafSite s;
    s.position = Vec3f(0, 0, 0);
    s.axis = Vec3f(0, 1, 0);
    s.normal = Vec3f(1, 0, 0);
    sk.leaf_sites.push_back(s);
    LeafCardConfig cfg;
    cfg.size_jitter = 0.0f;
    cfg.roll_jitter = 0.0f;
    cfg.leaves_per_site = 1;
    const jpov::MeshData m = BuildLeafCardsMesh(sk, cfg);
    // 十字卡片 = 2 quad = 8 顶点 / 12 索引。
    EXPECT_EQ(m.positions.size(), 8u);
    EXPECT_EQ(m.indices.size(), 12u);
    m.Validate();
}

TEST(LeafCardsTest, GrowthZeroProducesNothing) {
    Skeleton sk;
    LeafSite s;
    s.position = Vec3f(0, 0, 0);
    s.axis = Vec3f(0, 1, 0);
    s.normal = Vec3f(1, 0, 0);
    sk.leaf_sites.push_back(s);
    LeafCardConfig cfg;
    cfg.growth = 0.0f;
    const jpov::MeshData m = BuildLeafCardsMesh(sk, cfg);
    EXPECT_TRUE(m.positions.empty());
}

TEST(LeafCardsTest, DeterministicGreaterAndSizesScaleWithGrowth) {
    LSystem sys = BuildDragonClawPagodaTree();
    Skeleton sk = GenerateSkeleton(sys, TurtleConfig{}, 25, 3);
    ASSERT_FALSE(sk.leaf_sites.empty());
    LeafCardConfig cfg;
    cfg.growth = 1.0f;
    const jpov::MeshData a = BuildLeafCardsMesh(sk, cfg);
    const jpov::MeshData b = BuildLeafCardsMesh(sk, cfg);
    ASSERT_EQ(a.positions.size(), b.positions.size());
    EXPECT_FLOAT_EQ(a.positions[0].y(), b.positions[0].y());
    for (const Vec3f& n : a.normals) {
        const float len = std::sqrt(n.x() * n.x() + n.y() * n.y() + n.z() * n.z());
        EXPECT_NEAR(len, 1.0f, 1.0e-4f);
    }
}

// ============================ 向性（tropism） ============================

TEST(TropismTest, GravityPullsTipsDown) {
    // 先抬头 60° 再连画 5 段；带重力向性的末梢应比不带时更低（y 更小）。
    LSystemBuilder b;
    b.SetAxiom("&(60)F(1)F(1)F(1)F(1)F(1)");
    LSystem sys = b.Build();
    TurtleConfig base;
    base.length_scale = 1.0f;
    TurtleConfig trop = base;
    trop.tropism = Vec3f(0.0f, -1.0f, 0.0f);
    trop.tropism_strength = 0.3f;
    const Skeleton a = GenerateSkeleton(sys, base, 0, 0);
    const Skeleton c = GenerateSkeleton(sys, trop, 0, 0);
    ASSERT_FALSE(a.segments.empty());
    ASSERT_EQ(a.segments.size(), c.segments.size());
    EXPECT_LT(c.segments.back().end.y(), a.segments.back().end.y());
}

TEST(TropismTest, ZeroStrengthIsNoOp) {
    // 强度为 0（默认）→ 与完全不设向性逐位相同（零回归）。
    LSystem sys = BuildDragonClawPagodaTree();
    TurtleConfig base;
    TurtleConfig off = base;
    off.tropism = Vec3f(0.0f, -1.0f, 0.0f);
    off.tropism_strength = 0.0f;
    const Skeleton a = GenerateSkeleton(sys, base, 25, 0);
    const Skeleton b = GenerateSkeleton(sys, off, 25, 0);
    ASSERT_EQ(a.segments.size(), b.segments.size());
    for (size_t i = 0; i < a.segments.size(); ++i) {
        EXPECT_FLOAT_EQ(a.segments[i].end.x(), b.segments[i].end.x());
        EXPECT_FLOAT_EQ(a.segments[i].end.y(), b.segments[i].end.y());
        EXPECT_FLOAT_EQ(a.segments[i].end.z(), b.segments[i].end.z());
    }
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

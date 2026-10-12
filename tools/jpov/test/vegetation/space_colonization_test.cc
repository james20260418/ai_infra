// 空间殖民（Space Colonization）—— 单元测试（GL-free、纯 CPU）。
//
// 覆盖：确定性、吸引点被吞噬的收敛、结构不变量（枝链覆盖全部边 / 领口缝合标记 /
// 叶位=末梢）、管道模型粗细范围、向性方向、树种包络差异。
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/src/vegetation/space_colonization.h"

namespace jpov {
namespace vegetation {
namespace {

using Vec3f = geom::Vec3<float>;

// 骨架包围盒尺寸。
Vec3f BBoxSize(const Skeleton& skel) {
    float mn[3] = {1e30f, 1e30f, 1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const Segment& s : skel.segments) {
        for (const Vec3f& p : {s.start, s.end}) {
            mn[0] = std::min(mn[0], p.x());
            mx[0] = std::max(mx[0], p.x());
            mn[1] = std::min(mn[1], p.y());
            mx[1] = std::max(mx[1], p.y());
            mn[2] = std::min(mn[2], p.z());
            mx[2] = std::max(mx[2], p.z());
        }
    }
    return Vec3f(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
}

// 一个跑得动的小配置（低点数，快）。
SpaceColonizationConfig SmallConfig() {
    SpaceColonizationConfig c = FirConfig();
    c.attractor_count = 400;
    c.max_nodes = 1200;
    return c;
}

bool SameSkeleton(const Skeleton& a, const Skeleton& b) {
    if (a.segments.size() != b.segments.size()) {
        return false;
    }
    if (a.polylines.size() != b.polylines.size()) {
        return false;
    }
    if (a.leaf_sites.size() != b.leaf_sites.size()) {
        return false;
    }
    for (size_t i = 0; i < a.segments.size(); ++i) {
        if (a.segments[i].start.x() != b.segments[i].start.x() ||
            a.segments[i].start.y() != b.segments[i].start.y() ||
            a.segments[i].start.z() != b.segments[i].start.z() ||
            a.segments[i].end.x() != b.segments[i].end.x() ||
            a.segments[i].end.y() != b.segments[i].end.y() ||
            a.segments[i].end.z() != b.segments[i].end.z() ||
            a.segments[i].radius != b.segments[i].radius) {
            return false;
        }
    }
    return true;
}

// ============================ 确定性 ============================

TEST(SpaceColonizationTest, SameConfigSameSeedIsDeterministic) {
    const SpaceColonizationConfig c = SmallConfig();
    const Skeleton a = Colonize(c);
    const Skeleton b = Colonize(c);
    EXPECT_TRUE(SameSkeleton(a, b));
    EXPECT_GT(a.segments.size(), 0u);
}

TEST(SpaceColonizationTest, DifferentSeedGivesDifferentTree) {
    SpaceColonizationConfig c = SmallConfig();
    const Skeleton a = Colonize(c);
    c.seed = 999;
    const Skeleton b = Colonize(c);
    // 吸引点云不同 ⇒ 骨架不同（点数或位置至少一处不同）。
    EXPECT_FALSE(SameSkeleton(a, b));
}

// ============================ 收敛 / 结构 ============================

TEST(SpaceColonizationTest, TerminatesAndGrowsABranchingTree) {
    const Skeleton skel = Colonize(SmallConfig());
    EXPECT_GT(skel.segments.size(), 10u) << "应长出多段枝干";
    EXPECT_GT(skel.polylines.size(), 1u) << "应出现分叉（多于一条枝链）";
}

TEST(SpaceColonizationTest, PolylinesCoverEveryEdgeExactlyOnce) {
    // 结构不变量：除根外每个节点恰有一条入边，故每条边被一条枝链覆盖一次。
    const Skeleton skel = Colonize(SmallConfig());
    size_t covered_edges = 0;
    for (const Polyline& pl : skel.polylines) {
        ASSERT_GE(pl.points.size(), 2u);
        EXPECT_EQ(pl.points.size(), pl.radii.size());
        covered_edges += pl.points.size() - 1;
    }
    EXPECT_EQ(covered_edges, skel.segments.size());
}

TEST(SpaceColonizationTest, RootChainHasNoCollarButChildChainsDo) {
    const Skeleton skel = Colonize(SmallConfig());
    bool saw_root_chain = false;    // attach_radius == 0
    bool saw_child_chain = false;   // attach_radius > 0
    for (const Polyline& pl : skel.polylines) {
        if (pl.attach_radius == 0.0f) {
            saw_root_chain = true;
        } else if (pl.attach_radius > 0.0f) {
            saw_child_chain = true;
        } else {
            ADD_FAILURE() << "attach_radius 不应为负";
        }
    }
    EXPECT_TRUE(saw_root_chain) << "应存在根链（不缝合）";
    EXPECT_TRUE(saw_child_chain) << "应存在子链（带领口缝合参考半径）";
}

TEST(SpaceColonizationTest, LeafSitesOnTwigsAreUnitOriented) {
    SpaceColonizationConfig c = SmallConfig();
    c.leaves_on_terminal_twigs = true;
    const Skeleton skel = Colonize(c);
    // 叶位非空；每个叶位 axis 为单位向量、normal 与 axis 正交。
    ASSERT_GT(skel.leaf_sites.size(), 0u);
    for (const LeafSite& ls : skel.leaf_sites) {
        const float al = std::sqrt(ls.axis.x() * ls.axis.x() +
                                   ls.axis.y() * ls.axis.y() +
                                   ls.axis.z() * ls.axis.z());
        EXPECT_NEAR(al, 1.0f, 1.0e-3f);
        const float dot = ls.axis.x() * ls.normal.x() +
                          ls.axis.y() * ls.normal.y() +
                          ls.axis.z() * ls.normal.z();
        EXPECT_NEAR(dot, 0.0f, 1.0e-3f);
    }
    // 关掉叶位 → 无叶。
    c.leaves_on_terminal_twigs = false;
    EXPECT_EQ(Colonize(c).leaf_sites.size(), 0u);
}

TEST(SpaceColonizationTest, RadiiSpanBaseToMin) {
    // 管道模型：根最粗（=base_radius），末梢最细（=min_radius），且都落在 [min,base]。
    const SpaceColonizationConfig c = SmallConfig();
    const Skeleton skel = Colonize(c);
    ASSERT_GT(skel.segments.size(), 0u);
    // 根段 = 第一条 segment 的 start 半径不易取，改用 polyline 首点半径。
    float max_r = 0.0f;
    float min_r = 1e30f;
    for (const Polyline& pl : skel.polylines) {
        for (float r : pl.radii) {
            max_r = std::max(max_r, r);
            min_r = std::min(min_r, r);
        }
    }
    EXPECT_NEAR(max_r, c.base_radius, 1.0e-3f) << "最粗处应等于 base_radius";
    EXPECT_GE(min_r, c.min_radius - 1.0e-6f);
    EXPECT_LE(max_r, c.base_radius + 1.0e-6f);
}

// ============================ 向性 ============================

TEST(SpaceColonizationTest, TropismBendsGrowthTowardItsDirection) {
    SpaceColonizationConfig down = SmallConfig();
    down.tropism = Vec3f(0.0f, -1.0f, 0.0f);
    down.tropism_strength = 0.35f;
    SpaceColonizationConfig up = SmallConfig();
    up.tropism = Vec3f(0.0f, 1.0f, 0.0f);
    up.tropism_strength = 0.35f;
    SpaceColonizationConfig none = SmallConfig();
    none.tropism = Vec3f(0.0f, -1.0f, 0.0f);
    none.tropism_strength = 0.0f;   // 关闭 → 与无向性同

    const float h_down = BBoxSize(Colonize(down)).y();
    const float h_up = BBoxSize(Colonize(up)).y();
    // 上挺应比下垂高；且强度 0 时下垂配置应与"纯平均方向"（用 up 配置关掉）无关地确定。
    EXPECT_GT(h_up, h_down) << "向光上挺的树应比重力下垂的树高";
    // 关闭向性 = 零回归：与另一份"名义有向量但强度 0"完全一致。
    SpaceColonizationConfig none2 = SmallConfig();
    none2.tropism = Vec3f(0.0f, 1.0f, 0.0f);
    none2.tropism_strength = 0.0f;
    EXPECT_TRUE(SameSkeleton(Colonize(none), Colonize(none2)));
}

// ============================ 树种包络 ============================

TEST(SpaceColonizationTest, SpeciesHaveDistinctProportions) {
    const Vec3f fir = BBoxSize(Colonize(FirConfig()));
    const Vec3f shrub = BBoxSize(Colonize(ShrubConfig()));
    const Vec3f pine = BBoxSize(Colonize(PineConfig()));
    const Vec3f pag = BBoxSize(Colonize(DragonClawConfig()));
    // 杉高瘦（高 >> 宽）、灌木矮宽（高 < 宽）—— SC 应能区分轮廓级别。
    EXPECT_GT(fir.y(), shrub.y()) << "杉应比灌木高";
    EXPECT_GT(fir.y(), fir.x()) << "杉应为高瘦（高 > 冠幅）";
    EXPECT_LT(shrub.y(), shrub.x()) << "灌木应为矮宽（高 < 冠幅）";
    // 龙爪槐宽扁伞盖：冠幅明显大于高。
    EXPECT_GT(pag.x(), pag.y()) << "龙爪槐伞盖应宽于高";
    // 松有裸干：整体仍偏高。
    EXPECT_GT(pine.y(), 1.0f) << "松（裸干+冠）应高于 1m";
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

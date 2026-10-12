// JPOV PoseCache（pose_cache.h）纯 CPU 单测 —— 无 GL。
//
// 覆盖：时间戳严格递增校验（非递增忽略）、纯延迟查询（取不晚于 now-delay 的最近一条）、
//   delay=0 取最新、历史不够老给最早条、环形覆盖、ratio clamp、Clear。

#include "tools/jpov/interface/pose_cache.h"

#include <gtest/gtest.h>

namespace jpov {
namespace {

// ── 时间戳严格递增：非递增（相等/更小）采样被忽略 ──
TEST(PoseCacheTest, RequiresStrictlyIncreasingTime) {
    PoseCache c(8);
    EXPECT_TRUE(c.Push(1, 2, 0.0f, 0.0f));
    EXPECT_TRUE(c.Push(3, 4, 0.5f, 0.016f));
    EXPECT_FALSE(c.Push(5, 6, 0.7f, 0.016f)) << "相等时间戳应被忽略";
    EXPECT_FALSE(c.Push(5, 6, 0.7f, 0.010f)) << "更小时间戳应被忽略";
    EXPECT_EQ(c.size(), 2u);
    EXPECT_FLOAT_EQ(c.newest().t, 0.016f);
}

// ── 纯延迟查询：取「不晚于 now-delay 的最近一条」──
TEST(PoseCacheTest, QueryDelayedPicksLatestNotLaterThanTarget) {
    PoseCache c(32);
    for (int i = 0; i <= 10; ++i) {
        c.Push(/*a=*/i, /*b=*/i + 1, 0.0f, static_cast<float>(i) * 0.01f);
    }
    bool exact = false;
    // now=0.10，delay=0.05 → target=0.05 ⇒ 取 t=0.05（a=5）。
    const PoseSample s = c.QueryDelayed(0.10f, 0.05f, &exact);
    EXPECT_TRUE(exact);
    EXPECT_NEAR(s.t, 0.05f, 1e-6f);
    EXPECT_EQ(s.pose_a, 5);
    // now=0.10，delay=0 → 最新（t=0.10，a=10）。
    const PoseSample s0 = c.QueryDelayed(0.10f, 0.0f, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(s0.pose_a, 10);
    // now=0.10，delay=0.025 → target=0.075 ⇒ t=0.07（a=7，最近的 <=0.075）。
    const PoseSample s2 = c.QueryDelayed(0.10f, 0.025f, nullptr);
    EXPECT_NEAR(s2.t, 0.07f, 1e-6f);
    EXPECT_EQ(s2.pose_a, 7);
}

// ── 历史不够老（目标早于最早采样）→ 给最早那条，exact=false ──
TEST(PoseCacheTest, QueryWhenHistoryTooShortReturnsOldest) {
    PoseCache c(8);
    c.Push(1, 1, 0.0f, 0.0f);
    c.Push(2, 2, 0.0f, 0.01f);
    bool exact = true;
    const PoseSample s = c.QueryDelayed(0.10f, 0.5f, &exact);
    EXPECT_FALSE(exact);
    EXPECT_NEAR(s.t, 0.0f, 1e-6f);
    EXPECT_EQ(s.pose_a, 1);
}

// ── 环形覆盖：容量 3，推 5 条 → 只保留最后 3 条 ──
TEST(PoseCacheTest, RingOverwritesOldest) {
    PoseCache c(3);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(c.Push(i, i, 0.0f, static_cast<float>(i)));
    }
    EXPECT_EQ(c.size(), 3u);
    EXPECT_EQ(c.capacity(), 3u);
    EXPECT_EQ(c.oldest().pose_a, 2);
    EXPECT_EQ(c.newest().pose_a, 4);
    // delay 足够大 → 只能给最早（t=2）。
    const PoseSample s = c.QueryDelayed(4.0f, 10.0f, nullptr);
    EXPECT_EQ(s.pose_a, 2);
}

// ── ratio clamp 到 [0,1] ──
TEST(PoseCacheTest, RatioClamped) {
    PoseCache c(4);
    c.Push(0, 0, 2.0f, 0.0f);
    c.Push(0, 0, -1.0f, 0.1f);
    EXPECT_FLOAT_EQ(c.oldest().ratio, 1.0f);
    EXPECT_FLOAT_EQ(c.newest().ratio, 0.0f);
}

// ── Clear ──
TEST(PoseCacheTest, ClearResets) {
    PoseCache c(4);
    c.Push(1, 2, 0.5f, 0.0f);
    c.Clear();
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.size(), 0u);
    EXPECT_TRUE(c.Push(9, 9, 0.0f, 0.0f)) << "Clear 后时间戳校验应复位";
    EXPECT_EQ(c.newest().pose_a, 9);
}

}  // namespace
}  // namespace jpov

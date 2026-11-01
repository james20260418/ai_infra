// WindowedStat 单测：分窗统计的语义（关窗时机 / 读数保持 / 速率 / 均值极值）。

#include "tools/jpov/interface/windowed_stat.h"

#include <gtest/gtest.h>

TEST(WindowedStat, NoResultBeforeFirstWindowCloses) {
    jpov::WindowedStat stat(0.5);
    for (int i = 0; i < 10; ++i) {
        stat.Tick(static_cast<double>(i) * 0.01);   // 只过了 0.09s
    }
    EXPECT_FALSE(stat.has_result());
}

TEST(WindowedStat, RateUsesActualWindowElapsed) {
    jpov::WindowedStat stat(0.5);
    // t = 0, 1/60, ..., 29/60 共 30 次；t = 0.5 的那次触发关窗。
    for (int i = 0; i < 30; ++i) {
        stat.Tick(static_cast<double>(i) / 60.0);
    }
    EXPECT_FALSE(stat.has_result());
    stat.Tick(0.5);
    ASSERT_TRUE(stat.has_result());
    // 窗口 [0, 0.5) 共 30 次 ⇒ 60 次/秒。第 31 次属于新窗口。
    EXPECT_NEAR(stat.rate_per_second(), 60.0, 1e-9);
    EXPECT_EQ(stat.count(), 30);
}

TEST(WindowedStat, ResultHoldsUntilNextWindowCloses) {
    jpov::WindowedStat stat(0.5);
    for (int i = 0; i < 30; ++i) {
        stat.Tick(static_cast<double>(i) / 60.0);
    }
    stat.Tick(0.5);                       // 关窗 #1：60/s
    ASSERT_TRUE(stat.has_result());
    const double first = stat.rate_per_second();
    EXPECT_NEAR(first, 60.0, 1e-9);

    // 新窗口刚开始：读数必须**一点都不能动**（这就是"每 0.5s 才跳一次"）。
    stat.Tick(0.51);
    stat.Tick(0.52);
    EXPECT_DOUBLE_EQ(stat.rate_per_second(), first);

    // 窗口 [0.5, 1.0) 共 3 次（0.5 / 0.51 / 0.52）⇒ 6/s，下一个关窗点才更新。
    stat.Tick(1.0);
    EXPECT_NEAR(stat.rate_per_second(), 6.0, 1e-9);
    EXPECT_LT(stat.rate_per_second(), first);
}

TEST(WindowedStat, LongGapCountsOverRealElapsedTime) {
    jpov::WindowedStat stat(0.5);
    stat.Tick(0.0);
    EXPECT_FALSE(stat.has_result());
    stat.Tick(2.0);   // 2 秒没有采样
    ASSERT_TRUE(stat.has_result());
    // 窗口 [0, 2.0) 只有 1 次 ⇒ 0.5 次/秒（不是按 0.5s 名义窗口算出来的 2 次/秒）。
    EXPECT_NEAR(stat.rate_per_second(), 0.5, 1e-9);
}

TEST(WindowedStat, MeanMinMaxOnlyCoverValueSamples) {
    jpov::WindowedStat stat(0.5);
    stat.AddSample(10.0, 0.0);
    stat.Tick(0.1);              // 纯计数，不参与均值
    stat.AddSample(30.0, 0.2);
    stat.AddSample(20.0, 0.3);
    stat.Tick(0.5);              // 关窗（这次调用本身属于新窗口）
    ASSERT_TRUE(stat.has_result());
    EXPECT_DOUBLE_EQ(stat.mean(), 20.0);
    EXPECT_DOUBLE_EQ(stat.min_value(), 10.0);
    EXPECT_DOUBLE_EQ(stat.max_value(), 30.0);
    EXPECT_EQ(stat.count(), 4);   // 2 个带值 + 1 个带值 + 1 个纯 Tick
}

TEST(WindowedStat, TickOnlyWindowKeepsValueStatsZero) {
    jpov::WindowedStat stat(0.5);
    stat.Tick(0.0);
    stat.Tick(0.6);   // 关窗：只有计数，没有数值样本
    ASSERT_TRUE(stat.has_result());
    EXPECT_DOUBLE_EQ(stat.mean(), 0.0);
    EXPECT_DOUBLE_EQ(stat.max_value(), 0.0);
    EXPECT_DOUBLE_EQ(stat.min_value(), 0.0);
}

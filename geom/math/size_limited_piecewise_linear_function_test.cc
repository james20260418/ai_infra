// SizeLimitedPiecewiseLinearFunction 单元测试
//
// 覆盖：
//   1. AddSample / size / x() / y()，以及 x 非递增、容量溢出的 CHECK 崩溃
//   2. Evaluate：断点精确、区间内插值、两端线性外推
//   3. Evaluate(x, index_hint)：值正确 + hint 被写回 + 双指针（单调递增）推进
//   4. UpProp / DownProp：范围外无变化、未顶/压过线无变化、顶/压过则插入、与断点重合则抬点
//   5. 容量已满时 prop 不插入

#include "geom/math/size_limited_piecewise_linear_function.h"

#include "gtest/gtest.h"

namespace geom {
namespace math {

namespace {

// 建一条 y = x 的 4 采样折线：{0,1,2,3} -> {0,1,2,3}（3 段）。
// 容量取 8（有富余），便于验证 UpProp/DownProp 的插入。
SizeLimitedPiecewiseLinearFunction<8> MakeLinear() {
  SizeLimitedPiecewiseLinearFunction<8> f;
  for (int i = 0; i < 4; ++i) {
    f.AddSample(static_cast<double>(i), static_cast<double>(i));
  }
  return f;
}

}  // namespace

// ── AddSample / 访问 ──

TEST(SizeLimitedPwlTest, AddSampleAndAccess) {
  auto f = MakeLinear();
  EXPECT_EQ(f.size(), 4);
  EXPECT_DOUBLE_EQ(f.x(0), 0.0);
  EXPECT_DOUBLE_EQ(f.x(3), 3.0);
  EXPECT_DOUBLE_EQ(f.y(2), 2.0);
  f.Clear();
  EXPECT_EQ(f.size(), 0);
}

TEST(SizeLimitedPwlTest, AddSampleRejectsNonIncreasingX) {
  SizeLimitedPiecewiseLinearFunction<4> f;
  f.AddSample(0.0, 0.0);
  f.AddSample(1.0, 1.0);
  EXPECT_DEATH(f.AddSample(1.0, 9.0), "");   // 相等 → 非严格递增
  EXPECT_DEATH(f.AddSample(0.5, 9.0), "");   // 更小
}

TEST(SizeLimitedPwlTest, AddSampleRejectsOverflow) {
  SizeLimitedPiecewiseLinearFunction<2> f;
  f.AddSample(0.0, 0.0);
  f.AddSample(1.0, 1.0);
  EXPECT_DEATH(f.AddSample(2.0, 2.0), "");   // 容量 2 已满
}

// ── Evaluate ──

TEST(SizeLimitedPwlTest, EvaluateBreakpointsExact) {
  auto f = MakeLinear();
  EXPECT_DOUBLE_EQ(f.Evaluate(0.0), 0.0);
  EXPECT_DOUBLE_EQ(f.Evaluate(1.0), 1.0);
  EXPECT_DOUBLE_EQ(f.Evaluate(3.0), 3.0);
}

TEST(SizeLimitedPwlTest, EvaluateInterpolatesBetweenBreakpoints) {
  auto f = MakeLinear();
  EXPECT_DOUBLE_EQ(f.Evaluate(2.5), 2.5);
  EXPECT_DOUBLE_EQ(f(0.25), 0.25);   // operator() 同 Evaluate
}

TEST(SizeLimitedPwlTest, EvaluateExtrapolatesLinearly) {
  auto f = MakeLinear();
  EXPECT_DOUBLE_EQ(f.Evaluate(-1.0), -1.0);   // 左外推（首段斜率 1）
  EXPECT_DOUBLE_EQ(f.Evaluate(5.0), 5.0);     // 右外推（末段斜率 1）
}

TEST(SizeLimitedPwlTest, EvaluateNeedsTwoSamples) {
  SizeLimitedPiecewiseLinearFunction<4> f;
  f.AddSample(0.0, 0.0);
  EXPECT_DEATH(f.Evaluate(0.0), "");
}

// ── Evaluate(x, index_hint)：双指针 ──

TEST(SizeLimitedPwlTest, HintMatchesPlainEvaluate) {
  auto f = MakeLinear();
  int hint = 0;
  for (double x : {-1.0, 0.0, 0.5, 1.0, 2.7, 3.0, 4.5}) {
    EXPECT_DOUBLE_EQ(f.Evaluate(x, &hint), f.Evaluate(x)) << "x=" << x;
  }
}

TEST(SizeLimitedPwlTest, HintAdvancesMonotonically) {
  auto f = MakeLinear();
  int hint = 0;
  // 单调递增查询：hint 只应前进，且指向 x 所在区间。
  f.Evaluate(0.5, &hint);
  EXPECT_EQ(hint, 0);
  f.Evaluate(1.5, &hint);
  EXPECT_EQ(hint, 1);
  f.Evaluate(2.5, &hint);
  EXPECT_EQ(hint, 2);
  f.Evaluate(2.9, &hint);
  EXPECT_EQ(hint, 2);
}

TEST(SizeLimitedPwlTest, HintHandlesBackwardAndClamps) {
  auto f = MakeLinear();
  int hint = 2;
  // 向后查询 → 回扫到正确区间。
  EXPECT_DOUBLE_EQ(f.Evaluate(0.5, &hint), 0.5);
  EXPECT_EQ(hint, 0);
  // 进入时 hint 越界 → clamp。
  hint = 99;
  EXPECT_DOUBLE_EQ(f.Evaluate(2.5, &hint), 2.5);
  EXPECT_EQ(hint, 2);
  hint = -5;
  EXPECT_DOUBLE_EQ(f.Evaluate(0.5, &hint), 0.5);
  EXPECT_EQ(hint, 0);
}

// ── UpProp ──

TEST(SizeLimitedPwlTest, UpPropOutsideRangeNoChange) {
  auto f = MakeLinear();
  EXPECT_FALSE(f.UpProp(-1.0, 100.0));
  EXPECT_FALSE(f.UpProp(3.5, 100.0));
  EXPECT_EQ(f.size(), 4);
}

TEST(SizeLimitedPwlTest, UpPropBelowLineNoChange) {
  auto f = MakeLinear();
  EXPECT_FALSE(f.UpProp(1.5, 1.0));   // 线段值 = 1.5，立柱 1.0 顶不起来
  EXPECT_FALSE(f.UpProp(1.5, 1.5));   // 恰好等于也不变
  EXPECT_EQ(f.size(), 4);
}

TEST(SizeLimitedPwlTest, UpPropAboveLineInsertsPeak) {
  auto f = MakeLinear();
  ASSERT_TRUE(f.UpProp(1.5, 3.0));
  EXPECT_EQ(f.size(), 5);
  EXPECT_DOUBLE_EQ(f.x(2), 1.5);
  EXPECT_DOUBLE_EQ(f.y(2), 3.0);
  EXPECT_DOUBLE_EQ(f.Evaluate(1.5), 3.0);   // 折线经过被顶起的点
  EXPECT_DOUBLE_EQ(f.Evaluate(1.0), 1.0);   // 邻居不变
  EXPECT_DOUBLE_EQ(f.Evaluate(2.0), 2.0);
  EXPECT_DOUBLE_EQ(f.Evaluate(1.25), 2.0);  // (1,1)->(1.5,3) 中点
}

TEST(SizeLimitedPwlTest, UpPropOnBreakpointRaisesVertex) {
  auto f = MakeLinear();
  ASSERT_TRUE(f.UpProp(2.0, 5.0));
  EXPECT_EQ(f.size(), 4);                   // 与断点重合 → 不新增采样
  EXPECT_DOUBLE_EQ(f.y(2), 5.0);
  EXPECT_FALSE(f.UpProp(2.0, 4.0));         // 更低 → 无变化
  EXPECT_DOUBLE_EQ(f.y(2), 5.0);
}

TEST(SizeLimitedPwlTest, UpPropFullCapacityNoChange) {
  SizeLimitedPiecewiseLinearFunction<2> f;
  f.AddSample(0.0, 0.0);
  f.AddSample(10.0, 0.0);
  EXPECT_FALSE(f.UpProp(5.0, 3.0));         // 需要插入但容量满
  EXPECT_EQ(f.size(), 2);
}

// ── DownProp（对称）──

TEST(SizeLimitedPwlTest, DownPropSymmetric) {
  SizeLimitedPiecewiseLinearFunction<8> f;
  f.AddSample(0.0, 5.0);
  f.AddSample(10.0, 5.0);
  EXPECT_FALSE(f.DownProp(5.0, 6.0));       // 比线高 → 压不下去
  ASSERT_TRUE(f.DownProp(5.0, 1.0));        // 比线（5.0）低 → 压下去
  EXPECT_EQ(f.size(), 3);
  EXPECT_DOUBLE_EQ(f.Evaluate(5.0), 1.0);
  EXPECT_DOUBLE_EQ(f.Evaluate(0.0), 5.0);   // 邻居不变
  EXPECT_DOUBLE_EQ(f.Evaluate(10.0), 5.0);
}

TEST(SizeLimitedPwlTest, DownPropOnBreakpointLowersVertex) {
  auto f = MakeLinear();
  ASSERT_TRUE(f.DownProp(1.0, -2.0));
  EXPECT_EQ(f.size(), 4);
  EXPECT_DOUBLE_EQ(f.y(1), -2.0);
  EXPECT_FALSE(f.DownProp(1.0, 0.0));       // 更高 → 无变化
}

}  // namespace math
}  // namespace geom

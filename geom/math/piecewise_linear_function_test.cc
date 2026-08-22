// PiecewiseLinearFunction 单元测试
//
// 覆盖：
//   1. 构造校验——长度不一致 / 空序列 / x 非严格递增 都会 CHECK 崩溃
//   2. 断点处返回精确 y
//   3. 断点之间做线性插值
//   4. x 越界（小于最小 / 大于最大）用端点 y 夹断

#include "geom/math/piecewise_linear_function.h"

#include <vector>

#include "gtest/gtest.h"

namespace geom {
namespace math {

namespace {

// 三个断点：x = {0, 5, 10}, y = {0, 1, 0}
// 组成一个"山峰"：0~5 上升，5~10 下降，便于验证方向无关的插值。
const std::vector<double> kTestXs = {0.0, 5.0, 10.0};
const std::vector<double> kTestYs = {0.0, 1.0, 0.0};

}  // namespace

// ── 构造校验 ──

TEST(PiecewiseLinearFunctionTest, RejectsEmptyXs) {
    const std::vector<double> xs;
    const std::vector<double> ys;
    EXPECT_DEATH(PiecewiseLinearFunction<double> f(xs, ys), "");
}

TEST(PiecewiseLinearFunctionTest, RejectsMismatchedSizes) {
    const std::vector<double> xs = {0.0, 1.0, 2.0};
    const std::vector<double> ys = {0.0, 1.0};
    EXPECT_DEATH(PiecewiseLinearFunction<double> f(xs, ys), "");
}

TEST(PiecewiseLinearFunctionTest, RejectsNonIncreasingX) {
    // 相等（不严格递增）
    const std::vector<double> xs_equal = {0.0, 1.0, 1.0};
    const std::vector<double> ys = {0.0, 1.0, 2.0};
    EXPECT_DEATH(PiecewiseLinearFunction<double> f(xs_equal, ys), "");

    // 递减
    const std::vector<double> xs_decreasing = {0.0, -1.0, 2.0};
    EXPECT_DEATH(PiecewiseLinearFunction<double> f(xs_decreasing, ys), "");
}

// ── 单断点：常值函数 ──

TEST(PiecewiseLinearFunctionTest, SinglePointIsConstant) {
    const std::vector<double> xs = {3.0};
    const std::vector<double> ys = {7.0};
    const PiecewiseLinearFunction<double> f(xs, ys);

    EXPECT_DOUBLE_EQ(f(3.0), 7.0);
    EXPECT_DOUBLE_EQ(f(-100.0), 7.0);  // 越界夹断到端点
    EXPECT_DOUBLE_EQ(f(100.0), 7.0);
}

// ── 断点处精确值 ──

TEST(PiecewiseLinearFunctionTest, ExactBreakpoints) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    EXPECT_DOUBLE_EQ(f(0.0), 0.0);
    EXPECT_DOUBLE_EQ(f(5.0), 1.0);
    EXPECT_DOUBLE_EQ(f(10.0), 0.0);
}

// ── 区间内线性插值 ──

TEST(PiecewiseLinearFunctionTest, InterpolatesOnRisingSegment) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    // 0~5 上升段
    EXPECT_DOUBLE_EQ(f(2.5), 0.5);
    EXPECT_DOUBLE_EQ(f(1.0), 0.2);
    EXPECT_DOUBLE_EQ(f(4.9), 0.98);
}

TEST(PiecewiseLinearFunctionTest, InterpolatesOnFallingSegment) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    // 5~10 下降段
    EXPECT_DOUBLE_EQ(f(7.5), 0.5);
    EXPECT_DOUBLE_EQ(f(9.0), 0.2);
    EXPECT_DOUBLE_EQ(f(6.0), 0.8);
}

// ── 越界夹断 ──

TEST(PiecewiseLinearFunctionTest, ClampsBelowMin) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    EXPECT_DOUBLE_EQ(f(-1.0), 0.0);
    EXPECT_DOUBLE_EQ(f(-1000.0), 0.0);
}

TEST(PiecewiseLinearFunctionTest, ClampsAboveMax) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    EXPECT_DOUBLE_EQ(f(11.0), 0.0);
    EXPECT_DOUBLE_EQ(f(1000.0), 0.0);
}

// ── 边界值恰好等于断点时仍精确 ──

TEST(PiecewiseLinearFunctionTest, BoundaryQueryEqualsEndpoint) {
    const std::vector<double> xs = {-10.0, 0.0, 10.0};
    const std::vector<double> ys = {3.0, -2.0, 5.0};
    const PiecewiseLinearFunction<double> f(xs, ys);

    // 查询值恰好是最小/最大断点
    EXPECT_DOUBLE_EQ(f(-10.0), 3.0);
    EXPECT_DOUBLE_EQ(f(10.0), 5.0);
}

// ── 访问器 ──

TEST(PiecewiseLinearFunctionTest, ExposesBreakpoints) {
    const PiecewiseLinearFunction<double> f(kTestXs, kTestYs);
    EXPECT_EQ(f.xs().size(), static_cast<size_t>(3));
    EXPECT_EQ(f.ys().size(), static_cast<size_t>(3));
    EXPECT_DOUBLE_EQ(f.xs()[1], 5.0);
    EXPECT_DOUBLE_EQ(f.ys()[1], 1.0);
}

// ── y 值模板类型：float 支持 ──

TEST(PiecewiseLinearFunctionTest, SupportsFloatY) {
    const std::vector<double> xs = {0.0, 2.0, 4.0};
    const std::vector<float> ys = {0.0f, 1.0f, 0.0f};
    const PiecewiseLinearFunction<float> f(xs, ys);

    EXPECT_FLOAT_EQ(f(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(f(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(f(4.0f), 0.0f);
    EXPECT_FLOAT_EQ(f(1.0f), 0.5f);   // 插值
    EXPECT_FLOAT_EQ(f(3.0f), 0.5f);
    EXPECT_FLOAT_EQ(f(-100.0f), 0.0f);  // 越界夹断
    EXPECT_FLOAT_EQ(f(100.0f), 0.0f);
}

TEST(PiecewiseLinearFunctionTest, SupportsFloatYClampAndEndpoint) {
    const std::vector<double> xs = {0.0, 1.0};
    const std::vector<float> ys = {2.0f, 4.0f};
    const PiecewiseLinearFunction<float> f(xs, ys);

    // 端点精确
    EXPECT_FLOAT_EQ(f(0.0f), 2.0f);
    EXPECT_FLOAT_EQ(f(1.0f), 4.0f);
    // 区间中点
    EXPECT_FLOAT_EQ(f(0.5f), 3.0f);
    // 越界
    EXPECT_FLOAT_EQ(f(-5.0f), 2.0f);
    EXPECT_FLOAT_EQ(f(5.0f), 4.0f);
}

}  // namespace math
}  // namespace geom

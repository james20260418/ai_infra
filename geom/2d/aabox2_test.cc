#include "geom/2d/aabox2.h"
#include "gtest/gtest.h"

namespace geom {

class AABox2Test : public ::testing::Test {
 protected:
  void SetUp() override {}
};

// Test construction
TEST_F(AABox2Test, Construction) {
  AABox2d box(0.0, 10.0, -5.0, 5.0);
  EXPECT_DOUBLE_EQ(box.x_min(), 0.0);
  EXPECT_DOUBLE_EQ(box.x_max(), 10.0);
  EXPECT_DOUBLE_EQ(box.y_min(), -5.0);
  EXPECT_DOUBLE_EQ(box.y_max(), 5.0);
}

// Test default construction
TEST_F(AABox2Test, DefaultConstruction) {
  AABox2d box;
  EXPECT_DOUBLE_EQ(box.x_min_, 0.0);
  EXPECT_DOUBLE_EQ(box.x_max_, 0.0);
  EXPECT_DOUBLE_EQ(box.y_min_, 0.0);
  EXPECT_DOUBLE_EQ(box.y_max_, 0.0);
}

// Test spans
TEST_F(AABox2Test, Spans) {
  AABox2d box(-3.0, 7.0, 2.0, 12.0);
  EXPECT_DOUBLE_EQ(box.XSpan(), 10.0);
  EXPECT_DOUBLE_EQ(box.YSpan(), 10.0);
}

// Test zero-width box (valid, x_min == x_max)
TEST_F(AABox2Test, ZeroWidthBox) {
  AABox2d box(5.0, 5.0, 0.0, 10.0);
  EXPECT_DOUBLE_EQ(box.x_min(), 5.0);
  EXPECT_DOUBLE_EQ(box.x_max(), 5.0);
  EXPECT_DOUBLE_EQ(box.XSpan(), 0.0);
  EXPECT_DOUBLE_EQ(box.YSpan(), 10.0);
}

// Test point box (x_min == x_max && y_min == y_max)
TEST_F(AABox2Test, PointBox) {
  AABox2d box(3.0, 3.0, 4.0, 4.0);
  EXPECT_DOUBLE_EQ(box.XSpan(), 0.0);
  EXPECT_DOUBLE_EQ(box.YSpan(), 0.0);
}

// Test negative coordinates
TEST_F(AABox2Test, NegativeCoordinates) {
  AABox2d box(-10.0, -2.0, -8.0, -1.0);
  EXPECT_DOUBLE_EQ(box.x_min(), -10.0);
  EXPECT_DOUBLE_EQ(box.x_max(), -2.0);
  EXPECT_DOUBLE_EQ(box.y_min(), -8.0);
  EXPECT_DOUBLE_EQ(box.y_max(), -1.0);
  EXPECT_DOUBLE_EQ(box.XSpan(), 8.0);
  EXPECT_DOUBLE_EQ(box.YSpan(), 7.0);
}

// Test template type with float
TEST_F(AABox2Test, FloatType) {
  AABox2<float> box(-1.5f, 3.5f, 0.0f, 2.0f);
  EXPECT_FLOAT_EQ(box.x_min(), -1.5f);
  EXPECT_FLOAT_EQ(box.x_max(), 3.5f);
  EXPECT_FLOAT_EQ(box.XSpan(), 5.0f);
  EXPECT_FLOAT_EQ(box.YSpan(), 2.0f);
}

// Test large values
TEST_F(AABox2Test, LargeValues) {
  AABox2d box(-1e6, 1e6, -2e6, 2e6);
  EXPECT_DOUBLE_EQ(box.XSpan(), 2e6);
  EXPECT_DOUBLE_EQ(box.YSpan(), 4e6);
}

// Test that AABox2d is the expected type alias
TEST_F(AABox2Test, TypeAlias) {
  AABox2d box(0, 1, 0, 1);
  // AABox2d should be AABox2<double>
  bool is_double = std::is_same<decltype(box.x_min_), double>::value;
  EXPECT_TRUE(is_double);
}

}  // namespace geom

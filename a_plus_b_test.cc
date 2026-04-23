#include "a_plus_b.h"

#include "glog/logging.h"
#include "gtest/gtest.h"

namespace demo {
namespace {

// Test case for basic addition functionality.
TEST(APlusBTest, BasicAddition) {
  LOG(INFO) << "running a plus b test - BasicAddition";
  EXPECT_EQ(APlusB(1, 2), 3);
  EXPECT_EQ(APlusB(-1, 1), 0);
  EXPECT_EQ(APlusB(0, 0), 0);
}

// Test case for larger numbers.
TEST(APlusBTest, LargeNumbers) {
  LOG(INFO) << "running a plus b test - LargeNumbers";
  EXPECT_EQ(APlusB(1000, 2000), 3000);
  EXPECT_EQ(APlusB(-100, 200), 100);
}

// Test case for floating-point addition.
TEST(APlusBTest, FloatAddition) {
  LOG(INFO) << "running a plus b test - FloatAddition";
  EXPECT_FLOAT_EQ(APlusB(1.5f, 2.5f), 4.0f);
  EXPECT_FLOAT_EQ(APlusB(-1.2f, 3.7f), 2.5f);
  EXPECT_FLOAT_EQ(APlusB(0.0f, 0.0f), 0.0f);
}

// Test case for double-precision addition.
TEST(APlusBTest, DoubleAddition) {
  LOG(INFO) << "running a plus b test - DoubleAddition";
  EXPECT_DOUBLE_EQ(APlusB(1.25, 2.75), 4.0);
  EXPECT_DOUBLE_EQ(APlusB(-3.14, 1.59), -1.55);
  EXPECT_DOUBLE_EQ(APlusB(0.0, 0.0), 0.0);
}

}  // namespace
}  // namespace demo

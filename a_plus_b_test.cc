#include "a_plus_b.h"

#include "gtest/gtest.h"

namespace demo {
namespace {

// Test case for basic addition functionality.
TEST(APlusBTest, BasicAddition) {
  EXPECT_EQ(APlusB(1, 2), 3);
  EXPECT_EQ(APlusB(-1, 1), 0);
  EXPECT_EQ(APlusB(0, 0), 0);
}

// Test case for larger numbers.
TEST(APlusBTest, LargeNumbers) {
  EXPECT_EQ(APlusB(1000, 2000), 3000);
  EXPECT_EQ(APlusB(-100, 200), 100);
}

// Test case for template version with integers.
TEST(APlusBTest, TemplateInt) {
  EXPECT_EQ(APlusB(1, 2), 3);
  EXPECT_EQ(APlusB(-5, 10), 5);
}

// Test case for template version with double.
TEST(APlusBTest, TemplateDouble) {
  EXPECT_DOUBLE_EQ(APlusB(1.5, 2.5), 4.0);
  EXPECT_DOUBLE_EQ(APlusB(-0.1, 0.2), 0.1);
}

// Test case for template version with float.
TEST(APlusBTest, TemplateFloat) {
  EXPECT_FLOAT_EQ(APlusB(1.1f, 2.2f), 3.3f);
}

}  // namespace
}  // namespace demo
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

}  // namespace
}  // namespace demo
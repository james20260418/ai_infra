#include "geom/common/vec.h"
#include "gtest/gtest.h"

namespace geom {

// Test fixture for Vec tests
class VecTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Setup code if needed
  }
};

// Test construction
TEST_F(VecTest, Construction) {
  Vec2d a(1, 2);
  Vec3d b({3, 4, 5});
  Vec1d c;

  EXPECT_DOUBLE_EQ(a.x(), 1);
  EXPECT_DOUBLE_EQ(a.y(), 2);
  EXPECT_DOUBLE_EQ(b[0], 3);
  EXPECT_DOUBLE_EQ(b[1], 4);
  EXPECT_DOUBLE_EQ(b[2], 5);
  EXPECT_DOUBLE_EQ(c[0], 0);

  // Check trivially copyable
  static_assert(std::is_trivially_copyable<Vec2d>::value, 
                "Vec2d should be trivially copyable");
}

// Test algebra operations
TEST_F(VecTest, AlgebraOperations) {
  const Vec2d a(1, 2);
  const Vec2d b(3, 4);

  // operator +, +=
  const Vec2d sum = a + b;
  const Vec2d sum_gt(4, 6);
  EXPECT_EQ(sum, sum_gt);
  
  Vec2d a_t = a;
  a_t += b;
  EXPECT_EQ(a_t, sum_gt);

  // operator -, -=
  const Vec2d subs = a - b;
  const Vec2d subs_gt(-2, -2);
  EXPECT_EQ(subs, subs_gt);
  
  a_t = a;
  a_t -= b;
  EXPECT_EQ(a_t, subs_gt);

  // Scalar multiplication
  const Vec2d gain_1 = a * 4.0;
  const Vec2d gain_1_gt(4.0, 8.0);
  EXPECT_EQ(gain_1, gain_1_gt);
  
  const Vec2d gain_2 = 4.0 * a;
  EXPECT_EQ(gain_2, gain_1_gt);
  
  a_t = a;
  a_t *= 4.0;
  EXPECT_EQ(a_t, gain_1_gt);

  // Unary minus
  const Vec2d neg = -a;
  const Vec2d neg_gt(-1, -2);
  EXPECT_EQ(neg, neg_gt);
}

// Test dot product and norm
TEST_F(VecTest, DotProductAndNorm) {
  const Vec2d a(1, 2);
  const Vec2d b(3, 4);

  // Dot product
  double dot = a.Dot(b);
  EXPECT_DOUBLE_EQ(dot, 11.0);  // 1*3 + 2*4 = 11

  // Norm (magnitude)
  double norm = a.Norm();
  EXPECT_DOUBLE_EQ(norm, std::sqrt(5.0));  // sqrt(1^2 + 2^2) = sqrt(5)

  // Cross product (2D)
  double cross = a.Cross(b);
  EXPECT_DOUBLE_EQ(cross, -2.0);  // 1*4 - 2*3 = -2
}

// Test comparison operators
TEST_F(VecTest, ComparisonOperators) {
  const Vec2d a(1, 2);
  const Vec2d b(1, 2);
  const Vec2d c(1, 3);

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a == c);
}

// Test utility functions
TEST_F(VecTest, UtilityFunctions) {
  Vec2d a(3, 4);
  
  // Unit vector
  Vec2d unit = a.Unit();
  double norm = a.Norm();
  EXPECT_NEAR(unit.Norm(), 1.0, 1e-9);
  EXPECT_DOUBLE_EQ(unit.x(), 3.0 / norm);
  EXPECT_DOUBLE_EQ(unit.y(), 4.0 / norm);

  // Distance squared
  Vec2d b(0, 0);
  double dist_sq = a.DistanceSquareTo(b);
  EXPECT_DOUBLE_EQ(dist_sq, 25.0);  // 3^2 + 4^2 = 25

  // IsNear
  Vec2d c(3.000000001, 4.000000001);
  EXPECT_TRUE(a.IsNear(c, 1e-8));
  EXPECT_FALSE(a.IsNear(c, 1e-9));
}

// Test 3D vector operations
TEST_F(VecTest, Vec3dOperations) {
  Vec3d a(1, 2, 3);
  Vec3d b(4, 5, 6);

  // Addition
  Vec3d sum = a + b;
  EXPECT_DOUBLE_EQ(sum.x(), 5);
  EXPECT_DOUBLE_EQ(sum.y(), 7);
  EXPECT_DOUBLE_EQ(sum.z(), 9);

  // Dot product
  double dot = a.Dot(b);
  EXPECT_DOUBLE_EQ(dot, 32);  // 1*4 + 2*5 + 3*6 = 32

  // Cross product
  Vec3d cross = a.Cross(b);
  EXPECT_DOUBLE_EQ(cross.x(), -3);  // 2*6 - 3*5 = -3
  EXPECT_DOUBLE_EQ(cross.y(), 6);   // 3*4 - 1*6 = 6
  EXPECT_DOUBLE_EQ(cross.z(), -3);  // 1*5 - 2*4 = -3
}

// Test accessors and mutators
TEST_F(VecTest, AccessorsAndMutators) {
  Vec2d a(1, 2);
  
  // Accessors
  EXPECT_DOUBLE_EQ(a.x(), 1);
  EXPECT_DOUBLE_EQ(a.y(), 2);
  EXPECT_DOUBLE_EQ(a[0], 1);
  EXPECT_DOUBLE_EQ(a[1], 2);
  
  // Mutators
  a.x() = 3;
  a.y() = 4;
  EXPECT_DOUBLE_EQ(a.x(), 3);
  EXPECT_DOUBLE_EQ(a.y(), 4);
  
  a[0] = 5;
  a[1] = 6;
  EXPECT_DOUBLE_EQ(a[0], 5);
  EXPECT_DOUBLE_EQ(a[1], 6);
}

}  // namespace geom


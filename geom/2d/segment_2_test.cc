#include "geom/2d/segment_2.h"
#include "gtest/gtest.h"

namespace geom {

class Segment2Test : public ::testing::Test {
 protected:
  void SetUp() override {}
};

// Test default construction
TEST_F(Segment2Test, DefaultConstruction) {
  Segment2d seg;
  EXPECT_DOUBLE_EQ(seg.length(), 0.0);
  EXPECT_TRUE(seg.IsPoint());
}

// Test construction from two points
TEST_F(Segment2Test, ConstructionFromTwoPoints) {
  Vec2d start(0, 0);
  Vec2d end(3, 4);
  Segment2d seg(start, end);

  EXPECT_EQ(seg.start(), start);
  EXPECT_DOUBLE_EQ(seg.length(), 5.0);  // sqrt(3^2 + 4^2) = 5
  EXPECT_NEAR(seg.unit().Norm(), 1.0, 1e-9);
  EXPECT_DOUBLE_EQ(seg.unit().x(), 3.0 / 5.0);
  EXPECT_DOUBLE_EQ(seg.unit().y(), 4.0 / 5.0);
  EXPECT_FALSE(seg.IsPoint());
}

// Test construction from start + unit + length
TEST_F(Segment2Test, ConstructionFromUnitLength) {
  Vec2d start(1, 2);
  Vec2d unit(1, 0);
  Segment2d seg(start, unit, 10.0);

  EXPECT_EQ(seg.start(), start);
  EXPECT_EQ(seg.unit(), unit);
  EXPECT_DOUBLE_EQ(seg.length(), 10.0);
  EXPECT_DOUBLE_EQ(seg.end().x(), 11.0);
  EXPECT_DOUBLE_EQ(seg.end().y(), 2.0);
}

// Test end() computation
TEST_F(Segment2Test, EndPoint) {
  Segment2d seg(Vec2d(1, 2), Vec2d(4, 6));
  EXPECT_DOUBLE_EQ(seg.end().x(), 4.0);
  EXPECT_DOUBLE_EQ(seg.end().y(), 6.0);
}

// Test length getter/setter
TEST_F(Segment2Test, LengthSetter) {
  Segment2d seg(Vec2d(0, 0), Vec2d(3, 4));
  seg.set_length(10.0);
  EXPECT_DOUBLE_EQ(seg.length(), 10.0);
  // end() should reflect the new length
  EXPECT_DOUBLE_EQ(seg.end().x(), 6.0);
  EXPECT_DOUBLE_EQ(seg.end().y(), 8.0);
}

// Test ProjectOntoUnit
TEST_F(Segment2Test, ProjectOntoUnit) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  // Point at (5, 0) should project to 5.0
  EXPECT_DOUBLE_EQ(seg.ProjectOntoUnit(Vec2d(5, 0)), 5.0);
  // Point at (0, 5) should project to 0.0 (perpendicular)
  EXPECT_DOUBLE_EQ(seg.ProjectOntoUnit(Vec2d(0, 5)), 0.0);
  // Point before start projects negative
  EXPECT_LT(seg.ProjectOntoUnit(Vec2d(-1, 0)), 0.0);
  // Point after end projects > length
  EXPECT_GT(seg.ProjectOntoUnit(Vec2d(11, 0)), seg.length());
}

// Test ProductOntoUnit (Cross product)
TEST_F(Segment2Test, ProductOntoUnit) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  // Point directly on the line should have 0 cross product
  EXPECT_DOUBLE_EQ(seg.ProductOntoUnit(Vec2d(5, 0)), 0.0);
  // Point above the line should have positive cross product
  EXPECT_GT(seg.ProductOntoUnit(Vec2d(5, 1)), 0.0);
  // Point below the line should have negative cross product
  EXPECT_LT(seg.ProductOntoUnit(Vec2d(5, -1)), 0.0);
}

// Test ProductOntoUnit on vertical segment (from (0,0) to (0,5), unit = (0,1))
TEST_F(Segment2Test, ProductOntoUnitVertical) {
  Segment2d seg(Vec2d(0, 0), Vec2d(0, 5));
  // Cross product: unit (0,1) x (point - start) = 0*(y-0) - 1*(x-0) = -x
  // Point (1, 3): cross = -1, point is to the right
  EXPECT_DOUBLE_EQ(seg.ProductOntoUnit(Vec2d(1, 3)), -1.0);
  // Point (-2, 3): cross = 2, point is to the left
  EXPECT_DOUBLE_EQ(seg.ProductOntoUnit(Vec2d(-2, 3)), 2.0);
  // Point on the line: cross = 0
  EXPECT_DOUBLE_EQ(seg.ProductOntoUnit(Vec2d(0, 3)), 0.0);
}

// Test IsPointOn
TEST_F(Segment2Test, IsPointOn) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  // Point on segment
  EXPECT_TRUE(seg.IsPointOn(Vec2d(5, 0)));
  // Start point
  EXPECT_TRUE(seg.IsPointOn(Vec2d(0, 0)));
  // End point
  EXPECT_TRUE(seg.IsPointOn(Vec2d(10, 0)));
  // Point off the line
  EXPECT_FALSE(seg.IsPointOn(Vec2d(5, 1)));
  // Point on line but beyond segment
  EXPECT_FALSE(seg.IsPointOn(Vec2d(15, 0)));
  EXPECT_FALSE(seg.IsPointOn(Vec2d(-1, 0)));
}

// Test DistanceSquareTo
TEST_F(Segment2Test, DistanceSquareTo) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  // Point on segment
  EXPECT_DOUBLE_EQ(seg.DistanceSquareTo(Vec2d(5, 0)), 0.0);
  // Point above segment (perpendicular distance = 3)
  EXPECT_DOUBLE_EQ(seg.DistanceSquareTo(Vec2d(5, 3)), 9.0);
  // Point before start
  EXPECT_DOUBLE_EQ(seg.DistanceSquareTo(Vec2d(-3, 4)), 25.0);  // sqrt(3^2+4^2)=5
  // Point after end
  EXPECT_DOUBLE_EQ(seg.DistanceSquareTo(Vec2d(13, 4)), 25.0);  // sqrt(3^2+4^2)=5
}

// Test DistanceTo
TEST_F(Segment2Test, DistanceTo) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  EXPECT_DOUBLE_EQ(seg.DistanceTo(Vec2d(5, 3)), 3.0);
  EXPECT_DOUBLE_EQ(seg.DistanceTo(Vec2d(-3, 4)), 5.0);
}

// Test ShiftBy
TEST_F(Segment2Test, ShiftBy) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  seg.ShiftBy(Vec2d(5, 5));
  EXPECT_EQ(seg.start(), Vec2d(5, 5));
  EXPECT_DOUBLE_EQ(seg.end().x(), 15.0);
  EXPECT_DOUBLE_EQ(seg.end().y(), 5.0);
  // length should remain unchanged
  EXPECT_DOUBLE_EQ(seg.length(), 10.0);
}

// Test IsPoint with epsilon
TEST_F(Segment2Test, IsPointOnWithEpsilon) {
  Segment2d seg(Vec2d(0, 0), Vec2d(10, 0));
  // Slightly off the line but within epsilon
  EXPECT_TRUE(seg.IsPointOn(Vec2d(5, 1e-10), 1e-8));
  // Slightly off the line but outside epsilon
  EXPECT_FALSE(seg.IsPointOn(Vec2d(5, 1e-7), 1e-8));
}

// Test DebugString (just ensure it doesn't crash)
TEST_F(Segment2Test, DebugString) {
  Segment2d seg(Vec2d(0, 0), Vec2d(3, 4));
  std::string str = seg.DebugString();
  EXPECT_FALSE(str.empty());
}

// Test diagonal segment
TEST_F(Segment2Test, DiagonalSegment) {
  Vec2d start(1, 1);
  Vec2d end(4, 5);
  Segment2d seg(start, end);
  double expected_length = std::sqrt(3 * 3 + 4 * 4);  // = 5
  EXPECT_DOUBLE_EQ(seg.length(), expected_length);
  // Project midpoint
  Vec2d mid(2.5, 3.0);
  EXPECT_NEAR(seg.ProjectOntoUnit(mid), expected_length / 2.0, 1e-9);
  EXPECT_NEAR(seg.ProductOntoUnit(mid), 0.0, 1e-9);
}

// Test vertical segment
TEST_F(Segment2Test, VerticalSegment) {
  Segment2d seg(Vec2d(0, 0), Vec2d(0, 5));
  EXPECT_DOUBLE_EQ(seg.length(), 5.0);
  EXPECT_DOUBLE_EQ(seg.unit().x(), 0.0);
  EXPECT_DOUBLE_EQ(seg.unit().y(), 1.0);
  EXPECT_DOUBLE_EQ(seg.ProjectOntoUnit(Vec2d(0, 3)), 3.0);
}

}  // namespace geom

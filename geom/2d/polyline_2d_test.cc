#include <cmath>
#include <vector>

#include "geom/2d/polyline_2d.h"
#include "gtest/gtest.h"

namespace geom {

class Polyline2dTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  // Helper: create a simple 3-segment polyline
  // (0,0) -> (3,0) -> (3,4) -> (0,4)
  // segment lengths: 3, 4, 3
  // total length: 10
  std::vector<Vec2d> MakeSquarePath() const {
    return {Vec2d(0, 0), Vec2d(3, 0), Vec2d(3, 4), Vec2d(0, 4)};
  }
};

// Test construction
TEST_F(Polyline2dTest, Construction) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_EQ(poly.NumVertex(), 4);
  EXPECT_EQ(poly.NumSegment(), 3);
  EXPECT_DOUBLE_EQ(poly.Length(), 10.0);
}

// Test vertex points
TEST_F(Polyline2dTest, VertexPoints) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_EQ(poly.GetVertexPoint(0), Vec2d(0, 0));
  EXPECT_EQ(poly.GetVertexPoint(1), Vec2d(3, 0));
  EXPECT_EQ(poly.GetVertexPoint(2), Vec2d(3, 4));
  EXPECT_EQ(poly.GetVertexPoint(3), Vec2d(0, 4));
}

// Test vertex s values
TEST_F(Polyline2dTest, VertexSValues) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_DOUBLE_EQ(poly.GetVertexS(0), 0.0);
  EXPECT_DOUBLE_EQ(poly.GetVertexS(1), 3.0);
  EXPECT_DOUBLE_EQ(poly.GetVertexS(2), 7.0);
  EXPECT_DOUBLE_EQ(poly.GetVertexS(3), 10.0);
}

// Test segment lengths
TEST_F(Polyline2dTest, SegmentLengths) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_DOUBLE_EQ(poly.GetSegmentLength(0), 3.0);
  EXPECT_DOUBLE_EQ(poly.GetSegmentLength(1), 4.0);
  EXPECT_DOUBLE_EQ(poly.GetSegmentLength(2), 3.0);
}

// Test segment angles
TEST_F(Polyline2dTest, SegmentAngles) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_DOUBLE_EQ(poly.GetSegmentAngle(0), 0.0);               // right
  EXPECT_DOUBLE_EQ(poly.GetSegmentAngle(1), M_PI / 2.0);        // up
  // Left direction: NormalizeAngle(M_PI) returns -M_PI (normalized to [-pi, pi))
  EXPECT_DOUBLE_EQ(poly.GetSegmentAngle(2), -M_PI);             // left
}

// Test PointAtS at vertices
TEST_F(Polyline2dTest, PointAtSAtVertices) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_EQ(poly.PointAtS(0.0), Vec2d(0, 0));
  EXPECT_EQ(poly.PointAtS(3.0), Vec2d(3, 0));
  EXPECT_EQ(poly.PointAtS(7.0), Vec2d(3, 4));
  EXPECT_EQ(poly.PointAtS(10.0), Vec2d(0, 4));
}

// Test PointAtS at midpoints
TEST_F(Polyline2dTest, PointAtSMidpoints) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  // Midpoint of first segment
  EXPECT_EQ(poly.PointAtS(1.5), Vec2d(1.5, 0));
  // Midpoint of second segment
  EXPECT_EQ(poly.PointAtS(5.0), Vec2d(3, 2));
  // Midpoint of third segment
  EXPECT_EQ(poly.PointAtS(8.5), Vec2d(1.5, 4));
}

// Test PointAtS clamps out-of-range s values
TEST_F(Polyline2dTest, PointAtSClamping) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  // s < 0 should clamp to start
  EXPECT_EQ(poly.PointAtS(-5.0), Vec2d(0, 0));
  // s > Length should clamp to end
  EXPECT_EQ(poly.PointAtS(100.0), Vec2d(0, 4));
}

// Test GetSegmentIndex
TEST_F(Polyline2dTest, GetSegmentIndex) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_EQ(poly.GetSegmentIndex(0.0), 0);
  EXPECT_EQ(poly.GetSegmentIndex(1.5), 0);
  EXPECT_EQ(poly.GetSegmentIndex(3.0), 1);
  EXPECT_EQ(poly.GetSegmentIndex(5.0), 1);
  EXPECT_EQ(poly.GetSegmentIndex(7.0), 2);
  EXPECT_EQ(poly.GetSegmentIndex(9.0), 2);
}

// Test GetSegmentIndex with clamping
TEST_F(Polyline2dTest, GetSegmentIndexClamping) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  EXPECT_EQ(poly.GetSegmentIndex(-5.0), 0);   // clamps to 0
  EXPECT_EQ(poly.GetSegmentIndex(100.0), 2);  // clamps to last segment
}

// Test with a straight line (2 points)
TEST_F(Polyline2dTest, StraightLine) {
  std::vector<Vec2d> points = {Vec2d(0, 0), Vec2d(5, 0)};
  Polyline2d poly(points);

  EXPECT_EQ(poly.NumVertex(), 2);
  EXPECT_EQ(poly.NumSegment(), 1);
  EXPECT_DOUBLE_EQ(poly.Length(), 5.0);
  EXPECT_EQ(poly.PointAtS(2.5), Vec2d(2.5, 0));
}

// Test that vertex_points() const ref returns correctly
TEST_F(Polyline2dTest, VertexPointsAccessor) {
  auto points = MakeSquarePath();
  const Polyline2d poly(points);

  const auto& retrieved = poly.vertex_points();
  ASSERT_EQ(retrieved.size(), 4);
  EXPECT_EQ(retrieved[0], Vec2d(0, 0));
  EXPECT_EQ(retrieved[3], Vec2d(0, 4));
}

// Test multi-segment polyline with arbitrary points
TEST_F(Polyline2dTest, ArbitraryPath) {
  std::vector<Vec2d> points = {
    Vec2d(1, 1),
    Vec2d(4, 5),
    Vec2d(7, 2),
    Vec2d(10, 10)
  };
  Polyline2d poly(points);

  EXPECT_EQ(poly.NumVertex(), 4);
  // Check lengths are positive
  for (int i = 0; i < poly.NumSegment(); ++i) {
    EXPECT_GT(poly.GetSegmentLength(i), 0.0);
  }
  // Total length should equal sum of segment lengths
  double expected_total = 0.0;
  for (int i = 0; i < poly.NumSegment(); ++i) {
    expected_total += poly.GetSegmentLength(i);
  }
  EXPECT_NEAR(poly.Length(), expected_total, 1e-9);
}

// Test PointAtS returns values on the polyline
TEST_F(Polyline2dTest, PointAtSOnPolyline) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  for (double s = 0.0; s <= poly.Length(); s += 0.5) {
    Vec2d pt = poly.PointAtS(s);
    // The point should be on one of the segments
    bool on_segment = false;
    for (int i = 0; i < poly.NumSegment(); ++i) {
      Vec2d seg_start = poly.GetVertexPoint(i);
      Vec2d seg_end = poly.GetVertexPoint(i + 1);
      Segment2d seg(seg_start, seg_end);
      if (seg.IsPointOn(pt, 1e-9)) {
        on_segment = true;
        break;
      }
    }
    EXPECT_TRUE(on_segment) << "Point at s=" << s << " is not on any segment";
  }
}

// Test that s values are monotonic
TEST_F(Polyline2dTest, MonotonicSValues) {
  auto points = MakeSquarePath();
  Polyline2d poly(points);

  for (int i = 1; i < poly.NumVertex(); ++i) {
    EXPECT_GT(poly.GetVertexS(i), poly.GetVertexS(i - 1));
  }
}

}  // namespace geom

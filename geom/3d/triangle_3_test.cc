#include "geom/3d/triangle_3.h"

#include <optional>

#include "geom/common/vec.h"
#include "gtest/gtest.h"

namespace geom {

// ---- 构造 / 退化拒绝 ----

TEST(Triangle3Test, CreateValidTriangle) {
  std::optional<Triangle3d> t = Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0, 1, 0));
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->a(), Vec3d(0, 0, 0));
  EXPECT_EQ(t->b(), Vec3d(1, 0, 0));
  EXPECT_EQ(t->c(), Vec3d(0, 1, 0));
  // 顶点逆时针 → 法向 +z，且为单位向量。
  EXPECT_NEAR(t->normal().x(), 0.0, 1e-12);
  EXPECT_NEAR(t->normal().y(), 0.0, 1e-12);
  EXPECT_NEAR(t->normal().z(), 1.0, 1e-12);
  EXPECT_NEAR(t->normal().Norm(), 1.0, 1e-12);
}

TEST(Triangle3Test, CreateNormalOrientationFollowsWinding) {
  // 交换 b、c → 法向翻转（右手系）。
  std::optional<Triangle3d> t = Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(0, 1, 0), Vec3d(1, 0, 0));
  ASSERT_TRUE(t.has_value());
  EXPECT_NEAR(t->normal().z(), -1.0, 1e-12);
}

TEST(Triangle3Test, CreateRejectsCollinear) {
  // 三点共线。
  EXPECT_FALSE(Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(2, 0, 0)).has_value());
  // 斜向共线。
  EXPECT_FALSE(Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 1, 1), Vec3d(2, 2, 2)).has_value());
}

TEST(Triangle3Test, CreateRejectsCoincidentVertices) {
  // 两个顶点重合（此时三角形退化成一条线段）。
  EXPECT_FALSE(Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(1, 0, 0)).has_value());
  // 三个顶点重合。
  EXPECT_FALSE(Triangle3d::Create(Vec3d(1, 1, 1), Vec3d(1, 1, 1), Vec3d(1, 1, 1)).has_value());
}

TEST(Triangle3Test, CreateRejectsSliver) {
  // 面积趋近 0 的细长三角形（高 1e-9、底 1）：2*面积 = 1e-9 ≤ 1e-6 * 1。
  EXPECT_FALSE(
      Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0.5, 1e-9, 0)).has_value());
}

TEST(Triangle3Test, CreateAcceptsThinButValid) {
  // 高 1e-3 的窄三角形仍在阈值之上，应被接受（不能误伤网格里正常的细三角形）。
  EXPECT_TRUE(
      Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0.5, 1e-3, 0)).has_value());
}

// ---- 最近点（Ericson 分区）----

namespace {
// 标准测试三角形：A=(0,0,0)、B=(1,0,0)、C=(0,1,0)，位于 z=0 平面。
std::optional<Triangle3d> MakeRefTriangle() {
  return Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0, 1, 0));
}
}  // namespace

TEST(Triangle3Test, ClosestPointInFaceRegion) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  const Vec3d p(0.25, 0.25, -2.0);  // 投影落在三角形内部。
  EXPECT_EQ(t->ClosestPointTo(p), Vec3d(0.25, 0.25, 0));
  EXPECT_NEAR(t->DistanceTo(p), 2.0, 1e-12);
}

TEST(Triangle3Test, ClosestPointAtVertices) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->ClosestPointTo(Vec3d(-1.0, 0.0, 0.0)), Vec3d(0, 0, 0));  // 顶点 A
  EXPECT_EQ(t->ClosestPointTo(Vec3d(2.0, 0.0, 0.0)), Vec3d(1, 0, 0));   // 顶点 B
  EXPECT_EQ(t->ClosestPointTo(Vec3d(0.0, 2.0, 0.0)), Vec3d(0, 1, 0));   // 顶点 C
}

TEST(Triangle3Test, ClosestPointOnEdges) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->ClosestPointTo(Vec3d(0.5, -1.0, 0.0)), Vec3d(0.5, 0, 0));  // 边 AB
  EXPECT_EQ(t->ClosestPointTo(Vec3d(-1.0, 0.5, 0.0)), Vec3d(0, 0.5, 0));  // 边 AC
  EXPECT_EQ(t->ClosestPointTo(Vec3d(1.0, 1.0, 0.0)), Vec3d(0.5, 0.5, 0)); // 边 BC
}

TEST(Triangle3Test, ClosestPointOnSurfaceIsItself) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  // 三角形内若干点：最近点应为自己，距离为 0。
  const Vec3d samples[5] = {Vec3d(0, 0, 0), Vec3d(0.25, 0.25, 0), Vec3d(0.5, 0, 0), Vec3d(0, 0.5, 0),
      Vec3d(0.3, 0.3, 0)};
  for (int i = 0; i < 5; ++i) {
    EXPECT_NEAR(t->DistanceTo(samples[i]), 0.0, 1e-12);
    EXPECT_TRUE(t->ClosestPointTo(samples[i]).IsNear(samples[i], 1e-12));
  }
}

TEST(Triangle3Test, DistanceSquareToMatchesDistanceTo) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  const Vec3d p(0.25, 0.25, -3.0);
  EXPECT_NEAR(t->DistanceSquareTo(p), 9.0, 1e-12);
  EXPECT_NEAR(t->DistanceTo(p), 3.0, 1e-12);
}

TEST(Triangle3Test, DebugStringNotEmpty) {
  std::optional<Triangle3d> t = MakeRefTriangle();
  ASSERT_TRUE(t.has_value());
  EXPECT_FALSE(t->DebugString().empty());
}

}  // namespace geom

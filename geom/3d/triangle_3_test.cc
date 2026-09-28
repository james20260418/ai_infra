#include "geom/3d/triangle_3.h"

#include <cmath>
#include <optional>
#include <random>

#include "geom/common/common.h"
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

// 在三角形内均匀采样一点（重心坐标法）。
Vec3d SamplePointInTriangle(const Triangle3d& t, std::mt19937* seed) {
  const double r1 = std::sqrt(RandomDouble(0.0, 1.0, seed));
  const double r2 = RandomDouble(0.0, 1.0, seed);
  const double w_a = 1.0 - r1;
  const double w_b = r1 * (1.0 - r2);
  const double w_c = r1 * r2;
  return t.a() * w_a + t.b() * w_b + t.c() * w_c;
}

// 独立几何判据：q 是否落在三角形上/内（共面 + 三条边同侧）。
bool IsPointOnTriangle(const Triangle3d& t, const Vec3d& q, double eps) {
  const Vec3d n = t.normal();
  if (std::abs((q - t.a()).Dot(n)) > eps) {
    return false;
  }
  const Vec3d verts[3] = {t.a(), t.b(), t.c()};
  for (int i = 0; i < 3; ++i) {
    const Vec3d edge = verts[(i + 1) % 3] - verts[i];
    if (edge.Cross(q - verts[i]).Dot(n) < -eps) {
      return false;
    }
  }
  return true;
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

// 大规模随机锦标赛：随机三角形 + 随机查询点，验证 ClosestPointTo 的最近点
//   (a) 确实落在三角形内（独立几何判据，不依赖被测函数）；
//   (b) 到 p 的距离 = DistanceTo；
//   (c) 与「三角形内随机撒的 50 个点」PK：最近点不比它们中任何一个远。
// 1000 例 × 50 个竞争点。
TEST(Triangle3Test, RandomizedClosestPointTournament) {
  std::mt19937 seed(20260928);
  constexpr int kCases = 1000;
  constexpr int kCompetitors = 50;
  const double kEps = 1e-9;

  for (int c = 0; c < kCases; ++c) {
    // 随机非退化三角形（在 [-1,1]^3 内取三点，退化则重取）。
    std::optional<Triangle3d> tri;
    while (!tri.has_value()) {
      tri = Triangle3d::Create(
          Vec3d(RandomDouble(-1.0, 1.0, &seed), RandomDouble(-1.0, 1.0, &seed),
              RandomDouble(-1.0, 1.0, &seed)),
          Vec3d(RandomDouble(-1.0, 1.0, &seed), RandomDouble(-1.0, 1.0, &seed),
              RandomDouble(-1.0, 1.0, &seed)),
          Vec3d(RandomDouble(-1.0, 1.0, &seed), RandomDouble(-1.0, 1.0, &seed),
              RandomDouble(-1.0, 1.0, &seed)));
    }

    // 查询点放宽到 [-2,2]^3，覆盖内部、边角外侧等各种区域。
    const Vec3d p(RandomDouble(-2.0, 2.0, &seed), RandomDouble(-2.0, 2.0, &seed),
        RandomDouble(-2.0, 2.0, &seed));

    const Vec3d closest = tri->ClosestPointTo(p);
    const double dist_closest = (closest - p).Norm();

    // (a) 最近点必须在三角形内。
    EXPECT_TRUE(IsPointOnTriangle(*tri, closest, kEps))
        << "case " << c << ": 最近点不在三角形内" << closest.DebugString();
    // (b) 最近点到 p 的距离 = DistanceTo(p)。
    EXPECT_NEAR(dist_closest, tri->DistanceTo(p), 1e-9);
    // (c) PK：三角形内随机点的距离都不小于最近点距离。
    for (int k = 0; k < kCompetitors; ++k) {
      const Vec3d competitor = SamplePointInTriangle(*tri, &seed);
      EXPECT_LE(dist_closest, (competitor - p).Norm() + kEps)
          << "case " << c << ": 最近点比三角形内采样点还远";
    }
  }
}

}  // namespace geom

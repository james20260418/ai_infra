#include "geom/3d/triangle_matcher_3d.h"

#include <algorithm>
#include <optional>
#include <random>
#include <vector>

#include "geom/3d/triangle_3.h"
#include "geom/common/common.h"
#include "geom/common/vec.h"
#include "gtest/gtest.h"

namespace geom {
namespace {

// 单位立方体（[-0.5,0.5]^3）表面的 12 个三角形。
std::vector<Triangle3d> MakeUnitCubeTriangles() {
  const double h = 0.5;
  const Vec3d c000(-h, -h, -h);
  const Vec3d c100(h, -h, -h);
  const Vec3d c110(h, h, -h);
  const Vec3d c010(-h, h, -h);
  const Vec3d c001(-h, -h, h);
  const Vec3d c101(h, -h, h);
  const Vec3d c111(h, h, h);
  const Vec3d c011(-h, h, h);

  const Vec3d quads[12][3] = {
      {c001, c101, c111}, {c001, c111, c011},  // +z
      {c000, c010, c110}, {c000, c110, c100},  // -z
      {c100, c110, c111}, {c100, c111, c101},  // +x
      {c000, c001, c011}, {c000, c011, c010},  // -x
      {c010, c011, c111}, {c010, c111, c110},  // +y
      {c000, c100, c101}, {c000, c101, c001},  // -y
  };

  std::vector<Triangle3d> triangles;
  for (int i = 0; i < 12; ++i) {
    std::optional<Triangle3d> t = Triangle3d::Create(quads[i][0], quads[i][1], quads[i][2]);
    CHECK(t.has_value());
    triangles.push_back(*t);
  }
  return triangles;
}

// brute force：遍历全部三角形求最近者。返回下标，最近平方距离写入 out_sqr。
int BruteForceNearestIndex(const std::vector<Triangle3d>& triangles, const Vec3d& point,
    double* out_sqr /*output*/) {
  int best = 0;
  double best_sqr = triangles[0].DistanceSquareTo(point);
  for (int i = 1; i < static_cast<int>(triangles.size()); ++i) {
    const double d = triangles[i].DistanceSquareTo(point);
    if (d < best_sqr) {
      best_sqr = d;
      best = i;
    }
  }
  *out_sqr = best_sqr;
  return best;
}

bool ContainsIndex(const std::vector<int>& indices, int target) {
  for (const int idx : indices) {
    if (idx == target) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(TriangleMatcher3dTest, Accessors) {
  std::vector<Triangle3d> cube = MakeUnitCubeTriangles();
  TriangleMatcher3d<double> matcher(/*local_distance=*/0.05, /*grid_size=*/0.01, cube);
  EXPECT_DOUBLE_EQ(matcher.local_distance(), 0.05);
  EXPECT_DOUBLE_EQ(matcher.grid_size(), 0.01);
  EXPECT_EQ(matcher.triangles().size(), cube.size());
}

// 核心性质一：召回。凡距点 ≤ local_distance 的三角形，必须出现在 FindAllRecentTriangles 里。
TEST(TriangleMatcher3dTest, RecallMatchesBruteForceWithinLocalDistance) {
  std::vector<Triangle3d> cube = MakeUnitCubeTriangles();
  const double local_distance = 0.05;
  TriangleMatcher3d<double> matcher(local_distance, /*grid_size=*/0.01, cube);

  std::mt19937 seed(12345);
  int recalled_checks = 0;
  for (int iter = 0; iter < 20000; ++iter) {
    const Vec3d p(RandomDouble(-0.55, 0.55, &seed), RandomDouble(-0.55, 0.55, &seed),
        RandomDouble(-0.55, 0.55, &seed));
    const std::vector<int>& recent = matcher.FindAllRecentTriangles(p);
    for (int i = 0; i < static_cast<int>(cube.size()); ++i) {
      if (cube[i].DistanceTo(p) <= local_distance) {
        EXPECT_TRUE(ContainsIndex(recent, i)) << "召回失败：三角形 " << i << " 距点 "
                                              << cube[i].DistanceTo(p) << " 未被召回";
        ++recalled_checks;
      }
    }
  }
  // 保证确实有三角形落入 local_distance（否则上面循环空转，测不到东西）。
  EXPECT_GT(recalled_checks, 0);
}

// 核心性质二：最近邻。真正最近邻必须出现在 FindNearestTriangles 里；候选中的最小距离 = brute min。
TEST(TriangleMatcher3dTest, NearestMatchesBruteForce) {
  std::vector<Triangle3d> cube = MakeUnitCubeTriangles();
  TriangleMatcher3d<double> matcher(/*local_distance=*/0.05, /*grid_size=*/0.01, cube);

  std::mt19937 seed(20260928);
  int nonempty_checks = 0;
  for (int iter = 0; iter < 20000; ++iter) {
    const Vec3d p(RandomDouble(-0.6, 0.6, &seed), RandomDouble(-0.6, 0.6, &seed),
        RandomDouble(-0.6, 0.6, &seed));
    const std::vector<int>& near = matcher.FindNearestTriangles(p);
    if (near.empty()) {
      continue;
    }
    ++nonempty_checks;

    double brute_sqr = 0.0;
    const int brute_idx = BruteForceNearestIndex(cube, p, &brute_sqr);
    EXPECT_TRUE(ContainsIndex(near, brute_idx)) << "最近邻三角形 " << brute_idx << " 不在候选表里";

    double best_sqr = cube[near[0]].DistanceSquareTo(p);
    for (const int idx : near) {
      best_sqr = std::min(best_sqr, cube[idx].DistanceSquareTo(p));
    }
    EXPECT_NEAR(best_sqr, brute_sqr, 1e-15);
  }
  EXPECT_GT(nonempty_checks, 0);
}

// 负向哨兵：查询点距三角形 < local_distance，但其所在体素中心距三角形 0.045
// （> local_distance - 体素半径 = 0.0413）。若写入半径误用 "local_distance - 体素半径"，
// 该三角形不会被写入，此测试即 FAIL。用于守住文件头那条 "+体素半径" 的召回证明。
TEST(TriangleMatcher3dTest, RecallAtVoxelCenterBoundary) {
  std::vector<Triangle3d> triangles;
  std::optional<Triangle3d> t =
      Triangle3d::Create(Vec3d(0, 0, 0), Vec3d(0.02, 0, 0), Vec3d(0, 0.02, 0));
  ASSERT_TRUE(t.has_value());
  triangles.push_back(*t);

  TriangleMatcher3d<double> matcher(/*local_distance=*/0.05, /*grid_size=*/0.01, triangles);

  // 该点所在体素索引为 (0,0,4)，中心 (0.005,0.005,0.045)。
  const Vec3d p(0.005, 0.005, 0.049);
  EXPECT_NEAR(triangles[0].DistanceTo(p), 0.049, 1e-12);
  EXPECT_TRUE(ContainsIndex(matcher.FindAllRecentTriangles(p), 0));
  EXPECT_EQ(matcher.FindNearestTriangles(p).size(), 1u);
}

TEST(TriangleMatcher3dTest, FarPointReturnsEmpty) {
  std::vector<Triangle3d> cube = MakeUnitCubeTriangles();
  TriangleMatcher3d<double> matcher(/*local_distance=*/0.05, /*grid_size=*/0.01, cube);

  const Vec3d far(5.0, 5.0, 5.0);
  EXPECT_TRUE(matcher.FindAllRecentTriangles(far).empty());
  EXPECT_TRUE(matcher.FindNearestTriangles(far).empty());
}

TEST(TriangleMatcher3dTest, QueryIsDeterministicAndNonMutating) {
  std::vector<Triangle3d> cube = MakeUnitCubeTriangles();
  TriangleMatcher3d<double> matcher(/*local_distance=*/0.05, /*grid_size=*/0.01, cube);

  const Vec3d p(0.3, 0.3, 0.5);  // 落在 +z 面上：结果非空
  // 复制成值再比较（直接比较两个返回引用是自比，恒真）。
  const std::vector<int> nearest_before = matcher.FindNearestTriangles(p);
  const std::vector<int> recent_before = matcher.FindAllRecentTriangles(p);
  EXPECT_FALSE(nearest_before.empty());

  // 中间穿插别的查询：不得改变先前结果（返回引用共享内部表 → 防别名/污染）。
  matcher.FindNearestTriangles(Vec3d(0.5, 0.3, 0.3));
  matcher.FindAllRecentTriangles(Vec3d(0.3, 0.5, 0.3));
  matcher.FindNearestTriangles(Vec3d(-0.5, -0.3, -0.3));

  const std::vector<int> nearest_after = matcher.FindNearestTriangles(p);
  const std::vector<int> recent_after = matcher.FindAllRecentTriangles(p);
  EXPECT_EQ(nearest_before, nearest_after);
  EXPECT_EQ(recent_before, recent_after);
}

}  // namespace geom

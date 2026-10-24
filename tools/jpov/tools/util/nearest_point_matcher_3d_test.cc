// nearest_point_matcher_3d 的单测：与 brute force（遍历全部线段）逐点对照。
//
// 重点覆盖三类容易「看着对、其实错」的地方：
//   1) 环形扩张的停止判据 —— 用不同 cell_size 跑同一份数据，答案必须一模一样
//      （这类 bug 的典型症状就是「大格子看不出来、小格子全错」）；
//   2) 包围盒外查询点的夹回 —— 1e6 米外的点必须和近距离查询同样准；
//   3) 退化和等距 —— 单点折线、重复采样点、完全等距时的 tie-break。

#include "tools/jpov/tools/util/nearest_point_matcher_3d.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "geom/common/vec.h"
#include "gtest/gtest.h"

namespace jpov {
namespace util {
namespace {

using Vec3d = geom::Vec3<double>;

// 参照实现：遍历全部线段（折线只有一个点时按点算），返回最小距离与归属。
struct BruteForceResult {
  double distance = std::numeric_limits<double>::infinity();
  int polyline = -1;
  int segment = -1;
  double t = 0.0;
};

BruteForceResult BruteForce(const std::vector<std::vector<Vec3d>>& polylines, const Vec3d& q) {
  BruteForceResult best;
  for (int i = 0; i < static_cast<int>(polylines.size()); ++i) {
    const std::vector<Vec3d>& polyline = polylines[i];
    for (int k = 0; k + 1 < static_cast<int>(polyline.size()); ++k) {
      const Vec3d a = polyline[k];
      const Vec3d b = polyline[k + 1];
      const Vec3d ab = b - a;
      const double length_sqr = ab.Sqr();
      double t = 0.0;
      if (length_sqr > 0.0) {
        const Vec3d ap = q - a;
        const double dot = ap.x() * ab.x() + ap.y() * ab.y() + ap.z() * ab.z();
        t = std::clamp(dot / length_sqr, 0.0, 1.0);
      }
      const double distance = (q - (a + ab * t)).Norm();
      if (distance < best.distance) {
        best.distance = distance;
        best.polyline = i;
        best.segment = k;
        best.t = t;
      }
    }
  }
  return best;
}

std::vector<std::vector<Vec3d>> MakeRandomPolylines(int count, int min_points, int max_points,
                                                    double extent, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> coord(-extent, extent);
  std::uniform_int_distribution<int> point_count(min_points, max_points);
  std::vector<std::vector<Vec3d>> polylines;
  for (int i = 0; i < count; ++i) {
    std::vector<Vec3d> polyline;
    const int n = point_count(rng);
    for (int k = 0; k < n; ++k) {
      polyline.push_back(Vec3d(coord(rng), coord(rng), coord(rng)));
    }
    polylines.push_back(polyline);
  }
  return polylines;
}

// 核心对照：随机数据 + 多种 cell_size，结果必须与 brute force 一致。
TEST(NearestPointMatcher3dTest, MatchesBruteForceOnRandomData) {
  const std::vector<std::vector<Vec3d>> polylines =
      MakeRandomPolylines(/*count=*/120, /*min_points=*/2, /*max_points=*/8,
                          /*extent=*/4.0, /*seed=*/7);
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> coord(-6.0, 6.0);

  // cell_size 覆盖三种量级：远小于线段、接近线段、远大于线段。
  const double cell_sizes[] = { 0.03, 0.4, 5.0 };
  for (const double cell_size : cell_sizes) {
    const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/cell_size);
    EXPECT_EQ(matcher.num_polylines(), 120);
    EXPECT_GT(matcher.num_segments(), 0);
    for (int query_index = 0; query_index < 400; ++query_index) {
      const Vec3d query(coord(rng), coord(rng), coord(rng));
      const std::optional<NearestPointMatcher3d<double>::Match> match = matcher.Query(query);
      ASSERT_TRUE(match.has_value());
      const BruteForceResult reference = BruteForce(polylines, query);
      EXPECT_NEAR(match->distance, reference.distance, 1e-9)
          << "cell_size=" << cell_size << " query=" << query_index;
      // 等距时可能挑到另一条同样近的线段，但**距离**必须一致；这里再要求
      // 返回的点确实是自己报告的那条线段上的最近点。
      const std::vector<Vec3d>& polyline = polylines[match->polyline];
      const Vec3d a = polyline[match->segment];
      const Vec3d b = polyline[match->segment + 1];
      const Vec3d expected_point = a + (b - a) * match->t;
      EXPECT_NEAR((expected_point - match->point).Norm(), 0.0, 1e-12);
      EXPECT_GE(match->t, 0.0);
      EXPECT_LE(match->t, 1.0);
    }
  }
}

// 稀疏性：数据只占空间的一小部分时，占用格子数应远小于「包围盒 / cell_size」的稠密格数。
TEST(NearestPointMatcher3dTest, IsActuallySparse) {
  std::vector<std::vector<Vec3d>> polylines;
  for (int i = 0; i < 40; ++i) {
    std::vector<Vec3d> polyline;
    polyline.push_back(Vec3d(50.0 + i, 50.0, 50.0));
    polyline.push_back(Vec3d(50.0 + i, 50.0, 50.5));
    polylines.push_back(polyline);
  }
  const double cell_size = 0.1;
  const NearestPointMatcher3d<double> matcher(polylines, cell_size);
  // 包围盒 ~ 40×0.1×0.5 m ⇒ 稠密网格约 400×1×5 = 2000 格；实际只该占 240 格量级。
  EXPECT_LT(matcher.num_cells(), 400);
  EXPECT_GT(matcher.num_cells(), 0);
}

// 单点折线：segment 报告 -1，距离是点到点。
TEST(NearestPointMatcher3dTest, HandlesSinglePointPolylines) {
  std::vector<std::vector<Vec3d>> polylines;
  polylines.push_back({ Vec3d(0.0, 0.0, 0.0) });
  polylines.push_back({ Vec3d(10.0, 0.0, 0.0) });
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.5);

  const std::optional<NearestPointMatcher3d<double>::Match> match =
      matcher.Query(Vec3d(9.0, 0.0, 0.0));
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->polyline, 1);
  EXPECT_EQ(match->segment, -1);
  EXPECT_NEAR(match->distance, 1.0, 1e-12);
}

// 相邻采样点重合（零长线段）不能产生 0/0。
TEST(NearestPointMatcher3dTest, HandlesDuplicatePoints) {
  std::vector<std::vector<Vec3d>> polylines;
  polylines.push_back({ Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0) });
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.2);
  const std::optional<NearestPointMatcher3d<double>::Match> match =
      matcher.Query(Vec3d(0.0, 0.0, 3.0));
  ASSERT_TRUE(match.has_value());
  EXPECT_NEAR(match->distance, 3.0, 1e-12);
  EXPECT_EQ(match->t, 0.0);
}

// 包围盒外的远处查询：夹回盒内的优化必须不改变答案。
TEST(NearestPointMatcher3dTest, HandlesQueriesFarOutsideBounds) {
  const std::vector<std::vector<Vec3d>> polylines =
      MakeRandomPolylines(/*count=*/30, /*min_points=*/2, /*max_points=*/5,
                          /*extent=*/1.0, /*seed=*/3);
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.1);
  const double offsets[] = { 10.0, 1e3, 1e6 };
  for (const double offset : offsets) {
    const Vec3d query(offset, -offset, offset * 0.5);
    const std::optional<NearestPointMatcher3d<double>::Match> match = matcher.Query(query);
    ASSERT_TRUE(match.has_value());
    const BruteForceResult reference = BruteForce(polylines, query);
    EXPECT_NEAR(match->distance, reference.distance, 1e-6) << "offset=" << offset;
  }
}

// 完全等距的候选：必须取较小的线段下标（与遍历顺序无关）。
TEST(NearestPointMatcher3dTest, BreaksTiesBySegmentId) {
  std::vector<std::vector<Vec3d>> polylines;
  // 两条完全重合的线段，放在不同折线里 ⇒ 距离恒等。
  polylines.push_back({ Vec3d(0.0, 0.0, -1.0), Vec3d(0.0, 0.0, 1.0) });
  polylines.push_back({ Vec3d(0.0, 0.0, -1.0), Vec3d(0.0, 0.0, 1.0) });
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.25);
  for (int i = 0; i < 20; ++i) {
    const std::optional<NearestPointMatcher3d<double>::Match> match =
        matcher.Query(Vec3d(1.0, 0.0, 0.0));
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->polyline, 0);
  }
}

// 排除指定折线：查「这根折线的父折线」时必须用（否则距离恒为 0，命中自己）。
TEST(NearestPointMatcher3dTest, ExcludesRequestedPolyline) {
  std::vector<std::vector<Vec3d>> polylines;
  polylines.push_back({ Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0) });
  polylines.push_back({ Vec3d(1.0, 0.0, 0.0), Vec3d(1.0, 0.0, 1.0) });
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.25);
  const Vec3d query(0.0, 0.0, 0.5);

  // 不排除：命中自己那条（距离 0）。
  const std::optional<NearestPointMatcher3d<double>::Match> self = matcher.Query(query);
  ASSERT_TRUE(self.has_value());
  EXPECT_EQ(self->polyline, 0);
  EXPECT_NEAR(self->distance, 0.0, 1e-12);

  // 排除 0：只能命中另一条，距离 1 m。
  const std::optional<NearestPointMatcher3d<double>::Match> other =
      matcher.Query(query, /*exclude_polyline=*/0);
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->polyline, 1);
  EXPECT_NEAR(other->distance, 1.0, 1e-12);

  // 排除 1：回到自己。
  const std::optional<NearestPointMatcher3d<double>::Match> back =
      matcher.Query(query, /*exclude_polyline=*/1);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->polyline, 0);

  // 只有一条折线又排除它自己：正确的答案是「没有」。
  std::vector<std::vector<Vec3d>> single;
  single.push_back({ Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0) });
  const NearestPointMatcher3d<double> single_matcher(single, /*cell_size=*/0.25);
  EXPECT_FALSE(single_matcher.Query(Vec3d(0.0, 0.0, 0.5), /*exclude_polyline=*/0).has_value());
}

// 规模测试：4000 条折线 × 16 点（约 6 万线段），和 brute force 对照，
// 同时把两边耗时打出来（只打印，不做时间断言 —— 机器负载会影响计时）。
TEST(NearestPointMatcher3dTest, MatchesBruteForceAtScale) {
  const std::vector<std::vector<Vec3d>> polylines =
      MakeRandomPolylines(/*count=*/4000, /*min_points=*/16, /*max_points=*/16,
                          /*extent=*/9.0, /*seed=*/17);
  const NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.3);
  LOG(INFO) << "规模：折线 " << matcher.num_polylines() << " 线段 " << matcher.num_segments()
            << " 占用格子 " << matcher.num_cells();

  std::mt19937 rng(23);
  std::uniform_real_distribution<double> coord(-9.0, 9.0);
  std::vector<Vec3d> queries;
  for (int i = 0; i < 300; ++i) {
    queries.push_back(Vec3d(coord(rng), coord(rng), coord(rng)));
  }

  const std::chrono::steady_clock::time_point start_matcher = std::chrono::steady_clock::now();
  std::vector<double> matcher_distances;
  for (const Vec3d& query : queries) {
    const std::optional<NearestPointMatcher3d<double>::Match> match = matcher.Query(query);
    ASSERT_TRUE(match.has_value());
    matcher_distances.push_back(match->distance);
  }
  const std::chrono::steady_clock::time_point start_brute = std::chrono::steady_clock::now();
  std::vector<double> brute_distances;
  for (const Vec3d& query : queries) {
    brute_distances.push_back(BruteForce(polylines, query).distance);
  }
  const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

  const std::chrono::duration<double, std::milli> matcher_ms = start_brute - start_matcher;
  const std::chrono::duration<double, std::milli> brute_ms = end - start_brute;
  LOG(INFO) << "300 次查询：matcher " << matcher_ms.count() << " ms，brute force "
            << brute_ms.count() << " ms，加速 " << (brute_ms.count() / matcher_ms.count())
            << "×";

  ASSERT_EQ(matcher_distances.size(), brute_distances.size());
  for (std::size_t i = 0; i < matcher_distances.size(); ++i) {
    EXPECT_NEAR(matcher_distances[i], brute_distances[i], 1e-9) << "query " << i;
  }
}

// float 版本也要能编译并给出同样量级的结果（模板别只对 double 成立）。
TEST(NearestPointMatcher3dTest, WorksWithFloat) {
  std::vector<std::vector<geom::Vec3<float>>> polylines;
  polylines.push_back({ geom::Vec3<float>(0.0f, 0.0f, 0.0f),
                        geom::Vec3<float>(0.0f, 2.0f, 0.0f) });
  const NearestPointMatcher3d<float> matcher(polylines, /*cell_size=*/0.5f);
  const std::optional<NearestPointMatcher3d<float>::Match> match =
      matcher.Query(geom::Vec3<float>(1.0f, 1.0f, 0.0f));
  ASSERT_TRUE(match.has_value());
  EXPECT_NEAR(match->distance, 1.0f, 1e-5f);
}

}  // namespace
}  // namespace util
}  // namespace jpov

// ConvexHullTiles 单元测试 —— 用**完全不同的另一套算法** cross-validate。
//
// 被测：geom::math::ConvexHullCoveredTiles（PWL 上/下壳 + 扫描线）。
// 参照：独立实现——单调链凸包 + 逐 tile 矩形 vs 凸多边形 SAT。
//
// 覆盖：
//   1. 手算小例子（三角形）
//   2. 大量随机 8 点集：被测 vs 参照 的 tile 集合必须一致
//   3. 直接对照 ScreenHull 的上下界 vs 参照凸包的上/下界
//
// 注：随机用例里建的栅格必须**盖住**点范围，否则越界坐标会被夹断到边界格，
//     与参照（只遍历栅格内 tile）口径不一致。

#include "geom/math/convex_hull_tiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "gtest/gtest.h"

namespace geom {
namespace math {

namespace {

// ---------- 参照实现 A：单调链凸包（CCW，去共线） ----------
std::vector<Vec2d> RefHull(std::vector<Vec2d> p) {
  std::sort(p.begin(), p.end(), [](const Vec2d& a, const Vec2d& b) {
    return a.x != b.x ? a.x < b.x : a.y < b.y;
  });
  p.erase(std::unique(p.begin(), p.end(), [](const Vec2d& a, const Vec2d& b) {
            return a.x == b.x && a.y == b.y;
          }),
          p.end());
  if (p.size() < 3) return p;
  auto cross = [](const Vec2d& o, const Vec2d& a, const Vec2d& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  std::vector<Vec2d> h;
  for (const Vec2d& q : p) {   // 下链
    while (h.size() >= 2 && cross(h[h.size() - 2], h.back(), q) <= 0) h.pop_back();
    h.push_back(q);
  }
  const size_t lower = h.size() + 1;
  for (size_t i = p.size() - 1; i-- > 0;) {   // 上链
    while (h.size() >= lower && cross(h[h.size() - 2], h.back(), p[i]) <= 0) h.pop_back();
    h.push_back(p[i]);
  }
  h.pop_back();
  return h;
}

// ---------- 参照实现 B：矩形 vs 凸多边形 SAT ----------
bool RectPolyIntersect(double x0, double y0, double x1, double y1,
                       const std::vector<Vec2d>& poly) {
  if (poly.size() < 3) return false;
  auto sep = [&](double ax, double ay) -> bool {
    const double rx[4] = {x0, x1, x0, x1};
    const double ry[4] = {y0, y0, y1, y1};
    double rmin = 1e300, rmax = -1e300;
    for (int i = 0; i < 4; ++i) {
      const double q = ax * rx[i] + ay * ry[i];
      rmin = std::min(rmin, q);
      rmax = std::max(rmax, q);
    }
    double hmin = 1e300, hmax = -1e300;
    for (const Vec2d& v : poly) {
      const double q = ax * v.x + ay * v.y;
      hmin = std::min(hmin, q);
      hmax = std::max(hmax, q);
    }
    return (rmax < hmin) || (hmax < rmin);
  };
  if (sep(1, 0) || sep(0, 1)) return false;
  for (size_t i = 0; i < poly.size(); ++i) {
    const Vec2d& a = poly[i];
    const Vec2d& b = poly[(i + 1) % poly.size()];
    if (sep(-(b.y - a.y), (b.x - a.x))) return false;
  }
  return true;
}

std::vector<TileCoord> RefTiles(const TileGrid& g, const std::vector<Vec2d>& pts) {
  std::vector<TileCoord> out;
  const std::vector<Vec2d> hull = RefHull(pts);
  if (hull.size() < 3) return out;
  for (int r = 0; r < g.rows; ++r) {
    for (int c = 0; c < g.cols; ++c) {
      const double x0 = c * g.cell, y0 = r * g.cell;
      if (RectPolyIntersect(x0, y0, x0 + g.cell, y0 + g.cell, hull)) {
        out.push_back(TileCoord{c, r});
      }
    }
  }
  return out;
}

std::vector<TileCoord> Sorted(std::vector<TileCoord> v) {
  std::sort(v.begin(), v.end(), [](const TileCoord& a, const TileCoord& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// 参照凸包在 x 处的上/下界（扫所有边取最大/最小 y）。
void RefBoundsAt(const std::vector<Vec2d>& hull, double x, double* top, double* bot) {
  double xmin = 1e300, xmax = -1e300;
  for (const Vec2d& v : hull) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
  }
  x = std::max(xmin, std::min(xmax, x));   // clamp（端点浮点舍入会越界）
  double t = -1e300, b = 1e300;
  for (size_t i = 0; i < hull.size(); ++i) {
    const Vec2d& p = hull[i];
    const Vec2d& q = hull[(i + 1) % hull.size()];
    if (p.x == q.x) continue;
    const double xa = std::min(p.x, q.x), xb = std::max(p.x, q.x);
    if (x < xa || x > xb) continue;
    const double y = p.y + (q.y - p.y) * (x - p.x) / (q.x - p.x);
    t = std::max(t, y);
    b = std::min(b, y);
  }
  *top = t;
  *bot = b;
}

TileGrid MakeGrid(double cell, int cols, int rows) {
  TileGrid g;
  g.cell = cell;
  g.cols = cols;
  g.rows = rows;
  return g;
}

}  // namespace

// ── 手算小例子 ──

TEST(ConvexHullTilesTest, Triangle) {
  const std::vector<Vec2d> p = {{0, 0}, {10, 0}, {0, 10}};
  const TileGrid g = MakeGrid(1, 12, 12);
  EXPECT_TRUE(Sorted(ConvexHullCoveredTiles(g, p)) == RefTiles(g, p));
}

// ── 随机 cross-validate ──

TEST(ConvexHullTilesTest, RandomEightPointsMatchReference) {
  std::mt19937_64 rng(20261006);
  std::uniform_real_distribution<double> U(0.0, 32.0);
  const TileGrid g = MakeGrid(1.0, 32, 32);
  int mismatched = 0;
  for (int trial = 0; trial < 5000; ++trial) {
    std::vector<Vec2d> p(8);
    for (int i = 0; i < 8; ++i) p[i] = {U(rng), U(rng)};
    if (Sorted(ConvexHullCoveredTiles(g, p)) != RefTiles(g, p)) ++mismatched;
  }
  printf("random-8pts: %d/5000 sets mismatched\n", mismatched);
  EXPECT_EQ(mismatched, 0);
}

TEST(ConvexHullTilesTest, RandomEightPointsVariousGrids) {
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> U(0.0, 20.0);
  int mismatched = 0;
  for (double cell : {0.5, 1.0, 2.0, 3.0}) {
    const int n = static_cast<int>(std::ceil(20.0 / cell));   // 盖住点范围 [0,20]
    const TileGrid g = MakeGrid(cell, n, n);
    for (int trial = 0; trial < 2000; ++trial) {
      std::vector<Vec2d> p(8);
      for (int i = 0; i < 8; ++i) p[i] = {U(rng), U(rng)};
      if (Sorted(ConvexHullCoveredTiles(g, p)) != RefTiles(g, p)) ++mismatched;
    }
  }
  printf("random-8pts/various-cells: %d mismatched\n", mismatched);
  EXPECT_EQ(mismatched, 0);
}

TEST(ConvexHullTilesTest, RandomEightPointsLargeCells) {
  std::mt19937_64 rng(31);
  std::uniform_real_distribution<double> U(0.0, 192.0);
  const TileGrid g = MakeGrid(16.0, 12, 12);
  int mismatched = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    std::vector<Vec2d> p(8);
    for (int i = 0; i < 8; ++i) p[i] = {U(rng), U(rng)};
    if (Sorted(ConvexHullCoveredTiles(g, p)) != RefTiles(g, p)) ++mismatched;
  }
  printf("large-cell(16): %d mismatched\n", mismatched);
  EXPECT_EQ(mismatched, 0);
}

// ── ScreenHull 上下界 vs 参照 ──

TEST(ConvexHullTilesTest, HullBoundsMatchReference) {
  std::mt19937_64 rng(99);
  std::uniform_real_distribution<double> U(0.0, 32.0);
  int mismatched = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    std::vector<Vec2d> p(8);
    for (int i = 0; i < 8; ++i) p[i] = {U(rng), U(rng)};
    const std::vector<Vec2d> ref_hull = RefHull(p);
    if (ref_hull.size() < 3) continue;
    ScreenHull sh;
    sh.Build(p.data(), 8);
    ASSERT_TRUE(sh.valid());
    bool bad = false;
    for (int s = 0; s <= 20 && !bad; ++s) {
      const double x = sh.x_min() + (sh.x_max() - sh.x_min()) * s / 20.0;
      double rt = 0, rb = 0;
      RefBoundsAt(ref_hull, x, &rt, &rb);
      if (std::abs(ChainValueAt(sh.upper(), sh.upper_size(), x) - rt) > 1e-9 ||
          std::abs(ChainValueAt(sh.lower(), sh.lower_size(), x) - rb) > 1e-9) {
        bad = true;
      }
    }
    if (bad) ++mismatched;
  }
  printf("hull-bounds: %d/2000 sets mismatched\n", mismatched);
  EXPECT_EQ(mismatched, 0);
}

// ── 边界 / 契约 ──

TEST(ConvexHullTilesTest, EmptyAndDegenerateInputsYieldNothing) {
  const TileGrid g = MakeGrid(1, 16, 16);
  EXPECT_TRUE(ConvexHullCoveredTiles(g, {}).empty());                 // 空
  EXPECT_TRUE(ConvexHullCoveredTiles(g, {{5, 5}}).empty());           // 单点
  EXPECT_TRUE(ConvexHullCoveredTiles(g, {{5, 1}, {5, 9}}).empty());   // 全同 x
  EXPECT_TRUE(ConvexHullCoveredTiles(g, {{5, 5}, {5, 5}}).empty());   // 重复点
}

TEST(ConvexHullTilesTest, TooManyPointsIsRejected) {
  const TileGrid g = MakeGrid(1, 16, 16);
  std::vector<Vec2d> p(9);   // > kMaxPts(8)
  for (int i = 0; i < 9; ++i) p[i] = {static_cast<double>(i), static_cast<double>(i % 3)};
  std::vector<TileCoord> out;
  EXPECT_DEATH(AppendConvexHullCoveredTiles(g, p, &out), "");
}

TEST(ConvexHullTilesTest, AppendKeepsExistingAndStaysSortedUnique) {
  const TileGrid g = MakeGrid(1, 16, 16);
  std::vector<TileCoord> out = {{99, 99}};   // 预置一条 → 验证 append 不清空
  const std::vector<Vec2d> p = {{0, 0}, {10, 0}, {0, 10}};
  AppendConvexHullCoveredTiles(g, p, &out);
  ASSERT_GT(out.size(), 3u);
  EXPECT_TRUE((out[0] == TileCoord{99, 99}));
  // 追加段（index>=1）按 (col,row) 升序且无重复。
  for (size_t i = 2; i < out.size(); ++i) {
    const TileCoord& a = out[i - 1];
    const TileCoord& b = out[i];
    EXPECT_TRUE(a.x != b.x ? a.x < b.x : a.y < b.y);
  }
}

TEST(ConvexHullTilesTest, OffGridHullClampsToGrid) {
  const TileGrid g = MakeGrid(1, 10, 10);
  // 凸包远超网格 → 全部夹断到 [0,9]，不越界、不崩溃
  const std::vector<Vec2d> p = {{-100, -100}, {100, -100}, {100, 100}, {-100, 100}};
  const std::vector<TileCoord> t = Sorted(ConvexHullCoveredTiles(g, p));
  EXPECT_EQ(t.size(), 100u);   // 整屏 10x10
  for (const TileCoord& c : t) {
    EXPECT_GE(c.x, 0);
    EXPECT_LE(c.x, 9);
    EXPECT_GE(c.y, 0);
    EXPECT_LE(c.y, 9);
  }
}

TEST(ConvexHullTilesTest, TinyHullWithinOneTile) {
  const TileGrid g = MakeGrid(10.0, 8, 8);
  // 三点全落在 tile (1,1) = [10,20)x[10,20)
  const std::vector<Vec2d> p = {{12.0, 12.0}, {17.0, 13.0}, {14.0, 18.0}, {16.0, 16.0}};
  const std::vector<TileCoord> t = Sorted(ConvexHullCoveredTiles(g, p));
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE((t[0] == TileCoord{1, 1}));
}

TEST(ConvexHullTilesTest, VerticalEdgeMatchesReference) {
  // 左竖直边（同 x 两顶点）→ 折叠成单值函数，不得崩。
  // 用**非整数**坐标，避开「顶点正好落在 tile 边界」时 util(floor 半开) 与
  // 参照 SAT(擦边算相交) 的口径差（那是测度零的约定差，非缺陷）。
  const TileGrid g = MakeGrid(1, 12, 12);
  const std::vector<Vec2d> p = {{0.5, 0.5}, {0.5, 9.5}, {10.5, 5.5}};
  EXPECT_TRUE(Sorted(ConvexHullCoveredTiles(g, p)) == RefTiles(g, p));
}

TEST(ConvexHullTilesTest, BuildRejectsBadPointCount) {
  Vec2d p[16];
  for (int i = 0; i < 16; ++i) p[i] = {static_cast<double>(i), static_cast<double>(i)};
  EXPECT_DEATH({ ScreenHull h; h.Build(p, 0); }, "");
  EXPECT_DEATH({ ScreenHull h; h.Build(p, 9); }, "");
  EXPECT_DEATH({ ScreenHull h; h.Build(p, 16); }, "");
}

TEST(ConvexHullTilesTest, ChainValueAtInterpAndExtrapolate) {
  const Vec2d c[3] = {{0, 0}, {2, 4}, {4, 0}};
  EXPECT_DOUBLE_EQ(ChainValueAt(c, 3, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(ChainValueAt(c, 3, 1.0), 2.0);
  EXPECT_DOUBLE_EQ(ChainValueAt(c, 3, 3.0), 2.0);
  EXPECT_DOUBLE_EQ(ChainValueAt(c, 3, -1.0), -2.0);   // 左外推（斜率 2）
  EXPECT_DOUBLE_EQ(ChainValueAt(c, 3, 5.0), -2.0);    // 右外推（斜率 -2）
}

}  // namespace math
}  // namespace geom

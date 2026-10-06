// ConvexHullTiles — 屏幕空间凸包 → 它交叠的栅格（tile）集合
//
// 用途：给定一个 box 的屏幕投影点（≤ 8 个），快速求出「该凸包覆盖/交叠了哪些 tile」，
// 供 tile culling 用（替代「AABB 矩形」的保守覆盖，给出真轮廓）。
//
// 做法（context 无关、无 GL、可单测）：
//   1. 屏幕空间凸包：点按 x 排序，用**单调链（Andrew）**分别求上壳 / 下壳——
//      x 升序遍历、栈维护；遇到「凹折角」（中间点掉到两邻居连线下方 / 上方）就 pop
//      上一个点，直到折角正确。结果塞进 SizeLimitedPiecewiseLinearFunction。
//      （注：早先试过「极点 + 逐个 UpProp」，但 UpProp 只单向顶、不摘冗余点，会留
//       「凹坑」→ 不是真凸壳 → 改成这个标准栈法。）
//   2. 扫描线：先定列 (x) 范围，再逐列求该列内凸包的 [底, 顶]（= 列两端 + 中间夹住的
//      采样点），换算成行 (y) 范围，标该列内的 tile。
//
// 实现位置：geom/math/，命名空间 geom::math。

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "geom/common/check.h"
#include "geom/math/size_limited_piecewise_linear_function.h"

namespace geom {
namespace math {

// 2D 屏幕点（double）。
struct Vec2d {
  double x = 0.0;
  double y = 0.0;
};

// 等距栅格：单元边长 cell（> 0），列数 cols、行数 rows（>= 1）。
// 覆盖区域 = [0, cols*cell) × [0, rows*cell)；tile 索引 (x=列, y=行) ∈ [0,cols)×[0,rows)。
struct TileGrid {
  double cell = 1.0;
  int cols = 0;
  int rows = 0;
};

// tile 坐标（列, 行）。
struct TileCoord {
  int x = 0;
  int y = 0;
  bool operator==(const TileCoord& o) const { return x == o.x && y == o.y; }
};

// 屏幕空间凸包（上/下壳）。点集上限 kMaxPts（box = 8）。
class ScreenHull {
 public:
  static constexpr int kMaxPts = 8;

  // 由点集构造（单调链）。degenerate（有效点 < 2 或所有点同 x）时 valid()==false。
  // Pre-condition: 1 <= n <= kMaxPts。
  void Build(const Vec2d* pts, int n) {
    CHECK(n >= 1 && n <= kMaxPts) << "点数 " << n << " 越界 [1," << kMaxPts << "]";
    valid_ = false;
    // 排序（x 升、同 x 按 y 升）后去重。
    Vec2d s[kMaxPts];
    int m = 0;
    for (int i = 0; i < n; ++i) {
      s[m++] = pts[i];
    }
    std::sort(s, s + m, [](const Vec2d& a, const Vec2d& b) {
      return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    int w = 0;
    for (int i = 0; i < m; ++i) {
      if (i == 0 || s[i].x != s[i - 1].x || s[i].y != s[i - 1].y) {
        s[w++] = s[i];
      }
    }
    m = w;
    if (m < 2 || s[0].x == s[m - 1].x) {
      return;   // 退化（无横向跨度）
    }
    const auto cross = [](const Vec2d& o, const Vec2d& a, const Vec2d& b) {
      return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    // 下壳：x 升序，pop 直到左转（cross>0）。
    Vec2d lo[kMaxPts];
    int ln = 0;
    for (int i = 0; i < m; ++i) {
      while (ln >= 2 && cross(lo[ln - 2], lo[ln - 1], s[i]) <= 0) {
        --ln;
      }
      lo[ln++] = s[i];
    }
    // 上壳：x 降序，pop 直到左转（cross>0）；再反转成 x 升序。
    Vec2d up[kMaxPts];
    int un = 0;
    for (int i = m - 1; i >= 0; --i) {
      while (un >= 2 && cross(up[un - 2], up[un - 1], s[i]) <= 0) {
        --un;
      }
      up[un++] = s[i];
    }
    std::reverse(up, up + un);

    // 上/下壳可能含「同 x 的竖直边」→ PWL 要求 x 严格递增，故按 x 折叠：
    // 上壳同 x 取 y 最大，下壳取 y 最小（= 该 x 处的顶/底）。
    const auto fill = [&](const Vec2d* chain, int cn, bool upper,
                          SizeLimitedPiecewiseLinearFunction<kMaxPts>* f) {
      Vec2d c[kMaxPts];
      int on = 0;
      for (int i = 0; i < cn; ++i) {
        if (on > 0 && c[on - 1].x == chain[i].x) {
          c[on - 1].y = upper ? std::max(c[on - 1].y, chain[i].y)
                              : std::min(c[on - 1].y, chain[i].y);
        } else {
          c[on++] = chain[i];
        }
      }
      f->Clear();
      for (int i = 0; i < on; ++i) {
        f->AddSample(c[i].x, c[i].y);
      }
    };
    fill(up, un, /*upper=*/true, &upper_);
    fill(lo, ln, /*upper=*/false, &lower_);
    x_min_ = s[0].x;
    x_max_ = s[m - 1].x;
    valid_ = true;
  }

  bool valid() const { return valid_; }
  double x_min() const { return x_min_; }
  double x_max() const { return x_max_; }
  const SizeLimitedPiecewiseLinearFunction<kMaxPts>& upper() const { return upper_; }
  const SizeLimitedPiecewiseLinearFunction<kMaxPts>& lower() const { return lower_; }

 private:
  bool valid_ = false;
  double x_min_ = 0.0;
  double x_max_ = 0.0;
  SizeLimitedPiecewiseLinearFunction<kMaxPts> upper_;
  SizeLimitedPiecewiseLinearFunction<kMaxPts> lower_;
};

namespace internal {

inline int ClampIndex(double v, double cell, int n) {
  CHECK_GT(cell, 0.0);
  // 先算浮点索引再夹断，避免 (v/cell) 在 v 巨大时直接转 int 溢出。
  const double f = std::floor(v / cell);
  if (f <= 0.0) return 0;
  if (f >= static_cast<double>(n - 1)) return n - 1;
  return static_cast<int>(f);
}

// 该列（x ∈ [a, b]）内 hull 的 [底, 顶] = 列两端的值 + **列内部的顶点**。
// 注意：上壳是**凹**函数（∩），其**极大可能在列内部**（峰）；下壳凸，**极小可能在列内部**（谷）。
// 故列内顶点必须计入；但**不必每列重扫全部顶点**——用单调游标（up_vi/lo_vi）推进，均摊 O(1)。
inline void ColumnSpan(const ScreenHull& hull, double a, double b,
                       int* up_hint /*inout*/, int* lo_hint /*inout*/,
                       int* up_vi /*inout*/, int* lo_vi /*inout*/,
                       double* top /*output*/, double* bot /*output*/) {
  const SizeLimitedPiecewiseLinearFunction<ScreenHull::kMaxPts>& up = hull.upper();
  const SizeLimitedPiecewiseLinearFunction<ScreenHull::kMaxPts>& lo = hull.lower();
  double t = std::max(up.Evaluate(a, up_hint), up.Evaluate(b, up_hint));
  double bo = std::min(lo.Evaluate(a, lo_hint), lo.Evaluate(b, lo_hint));
  // 列内 (a,b) 的顶点：游标只前进（a 随列推进），故两个 while 合计均摊 O(1)。
  int i = *up_vi;
  while (i < up.size() && up.x(i) <= a) ++i;
  while (i < up.size() && up.x(i) < b) {
    t = std::max(t, up.y(i));
    ++i;
  }
  *up_vi = i;
  int j = *lo_vi;
  while (j < lo.size() && lo.x(j) <= a) ++j;
  while (j < lo.size() && lo.x(j) < b) {
    bo = std::min(bo, lo.y(j));
    ++j;
  }
  *lo_vi = j;
  *top = t;
  *bot = bo;
}

}  // namespace internal

// 求点集凸包交叠了哪些栅格 tile，**追加**到 *out（不清空）。
// 每个 tile 恰好 append 一次（列内按 row 升序；跨列按 col 升序），**天然无重复**。
// 点集上限 ScreenHull::kMaxPts；网格尺寸非法 / 点集退化 → 不追加。
// out 若预留足够容量则**零分配**（生产里可直接接到共享 tile 表 / 调用方缓冲）。
inline void AppendConvexHullCoveredTiles(const TileGrid& grid,
                                         const std::vector<Vec2d>& pts,
                                         std::vector<TileCoord>* out /*inout*/) {
  CHECK(out != nullptr);
  CHECK_GT(grid.cell, 0.0) << "cell 必须 > 0";
  CHECK_GE(grid.cols, 1);
  CHECK_GE(grid.rows, 1);
  if (pts.empty() || static_cast<int>(pts.size()) > ScreenHull::kMaxPts) {
    return;
  }
  ScreenHull hull;
  hull.Build(pts.data(), static_cast<int>(pts.size()));
  if (!hull.valid()) {
    return;
  }
  const int c0 = internal::ClampIndex(hull.x_min(), grid.cell, grid.cols);
  const int c1 = internal::ClampIndex(hull.x_max(), grid.cell, grid.cols);
  if (c0 > c1) {
    return;
  }
  int up_hint = 0;
  int lo_hint = 0;
  int up_vi = 0;   // 上壳顶点游标（单调）
  int lo_vi = 0;   // 下壳顶点游标（单调）
  const double hx0 = hull.x_min();
  const double hx1 = hull.x_max();
  for (int c = c0; c <= c1; ++c) {
    // 首/末列的列区间会超出凸包的 x 范围；**取交**，否则 Evaluate 会外推出界、多标 tile。
    const double a = std::max(static_cast<double>(c) * grid.cell, hx0);
    const double b = std::min(static_cast<double>(c + 1) * grid.cell, hx1);
    if (a > b) {
      continue;
    }
    double top = 0.0;
    double bot = 0.0;
    internal::ColumnSpan(hull, a, b, &up_hint, &lo_hint, &up_vi, &lo_vi, &top, &bot);
    const int r0 = internal::ClampIndex(bot, grid.cell, grid.rows);
    const int r1 = internal::ClampIndex(top, grid.cell, grid.rows);
    for (int r = r0; r <= r1; ++r) {
      out->push_back(TileCoord{c, r});
    }
  }
}

// 便捷：返回新 vector（每次调用一次分配）。
inline std::vector<TileCoord> ConvexHullCoveredTiles(const TileGrid& grid,
                                                     const std::vector<Vec2d>& pts) {
  std::vector<TileCoord> out;
  AppendConvexHullCoveredTiles(grid, pts, &out);
  return out;
}

}  // namespace math
}  // namespace geom

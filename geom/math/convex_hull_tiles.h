// ConvexHullTiles — 屏幕空间凸包 → 它交叠的栅格（tile）集合
//
// 用途：给定一个 box 的屏幕投影点（≤ 8 个），快速求出「该凸包覆盖/交叠了哪些 tile」，
// 供 tile culling 用（替代「AABB 矩形」的保守覆盖，给出真轮廓）。
//
// 做法（context 无关、无 GL、可单测）：
//   1. 屏幕空间凸包：8 点 → 取 x 最小/最大两点为左右极点 → 以极点为两端建
//      SizeLimitedPiecewiseLinearFunction → 其余点**依次 UpProp** 得上凸壳、
//      **DownProp** 得下凸壳。
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

namespace internal {
// 凸性清理（定义见下）：反复摘掉「不在两邻居连线上/下方」的冗余采样，收敛后即真凸壳。
template <int Cap>
void CleanupHull(SizeLimitedPiecewiseLinearFunction<Cap>* f, bool upper);
}  // namespace internal

// 屏幕空间凸包（上/下壳）。
// 点集上限 kMaxPts（box = 8）。
class ScreenHull {
 public:
  static constexpr int kMaxPts = 8;

  // 由点集构造。degenerate（点数 < 2 或所有点同 x）时 valid()==false。
  // 非极点按**x 升序**依次 UpProp(上壳) / DownProp(下壳)。
  // Pre-condition: 1 <= n <= kMaxPts。
  void Build(const Vec2d* pts, int n) {
    CHECK(n >= 1 && n <= kMaxPts) << "点数 " << n << " 越界 [1, " << kMaxPts << "]";
    valid_ = false;
    int i_l = 0, i_r = 0;
    for (int i = 1; i < n; ++i) {
      if (pts[i].x < pts[i_l].x) i_l = i;
      if (pts[i].x > pts[i_r].x) i_r = i;
    }
    if (pts[i_r].x <= pts[i_l].x) {
      return;   // 零宽（全同 x）→ 退化
    }
    // 上/下壳初值 = 左右极点的直线。
    upper_.Clear();
    lower_.Clear();
    upper_.AddSample(pts[i_l].x, pts[i_l].y);
    upper_.AddSample(pts[i_r].x, pts[i_r].y);
    lower_.AddSample(pts[i_l].x, pts[i_l].y);
    lower_.AddSample(pts[i_r].x, pts[i_r].y);
    // 其余点按 x 升序依次顶起 / 压下。
    int order[kMaxPts];
    int m = 0;
    for (int i = 0; i < n; ++i) {
      if (i != i_l && i != i_r) order[m++] = i;
    }
    std::sort(order, order + m, [&](int a, int b) { return pts[a].x < pts[b].x; });
    for (int k = 0; k < m; ++k) {
      const Vec2d p = pts[order[k]];
      upper_.UpProp(p.x, p.y);
      lower_.DownProp(p.x, p.y);
    }
    // ⚠️ UpProp/DownProp 只往一个方向顶、**不摘除冗余点** ⇒ 后插入的点会把先前的点
    //    变成「凹坑」（先前的点在两邻居连线下方）。故再跑一趟**凸性清理**：反复摘掉
    //    「不在两邻居连线上/下方」的冗余采样，收敛后即为真凸壳。
    internal::CleanupHull(&upper_, /*upper=*/true);
    internal::CleanupHull(&lower_, /*upper=*/false);
    x_min_ = pts[i_l].x;
    x_max_ = pts[i_r].x;
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

// 清理一条壳：反复摘除内部采样 i，若它不在两邻居连线的「上壳=上方 / 下壳=下方」侧。
// 收敛后折线即为该方向上的凸壳（去共线）。
template <int Cap>
void CleanupHull(SizeLimitedPiecewiseLinearFunction<Cap>* f, bool upper) {
  double xs[Cap];
  double ys[Cap];
  int n = 0;
  for (int i = 0; i < f->size(); ++i) {
    xs[n] = f->x(i);
    ys[n] = f->y(i);
    ++n;
  }
  bool changed = true;
  while (changed && n >= 3) {
    changed = false;
    for (int i = 1; i + 1 < n; ++i) {
      const double t = (xs[i] - xs[i - 1]) / (xs[i + 1] - xs[i - 1]);
      const double chord = ys[i - 1] + (ys[i + 1] - ys[i - 1]) * t;
      const bool redundant = upper ? (ys[i] <= chord) : (ys[i] >= chord);
      if (redundant) {
        for (int k = i; k + 1 < n; ++k) {
          xs[k] = xs[k + 1];
          ys[k] = ys[k + 1];
        }
        --n;
        changed = true;
        break;
      }
    }
  }
  f->Clear();
  for (int i = 0; i < n; ++i) {
    f->AddSample(xs[i], ys[i]);
  }
}

inline int ClampIndex(double v, double cell, int n) {
  CHECK_GT(cell, 0.0);
  // 先算浮点索引再夹断，避免 (v/cell) 在 v 巨大时直接转 int 溢出。
  const double f = std::floor(v / cell);
  if (f <= 0.0) return 0;
  if (f >= static_cast<double>(n - 1)) return n - 1;
  return static_cast<int>(f);
}

// 该列（x ∈ [a, b]）内 hull 的 [底, 顶]：取列两端 + 夹在列内的采样点。
inline void ColumnSpan(const ScreenHull& hull, double a, double b,
                       int* up_hint /*inout*/, int* lo_hint /*inout*/,
                       double* top /*output*/, double* bot /*output*/) {
  const SizeLimitedPiecewiseLinearFunction<ScreenHull::kMaxPts>& up = hull.upper();
  const SizeLimitedPiecewiseLinearFunction<ScreenHull::kMaxPts>& lo = hull.lower();
  double t = std::max(up.Evaluate(a, up_hint), up.Evaluate(b, up_hint));
  double bo = std::min(lo.Evaluate(a, lo_hint), lo.Evaluate(b, lo_hint));
  // 夹在本列 (a, b) 内的采样点（index 增长即表示跨过一个顶点）。
  for (int i = 0; i < up.size(); ++i) {
    const double xv = up.x(i);
    if (xv > a && xv < b) t = std::max(t, up.y(i));
  }
  for (int i = 0; i < lo.size(); ++i) {
    const double xv = lo.x(i);
    if (xv > a && xv < b) bo = std::min(bo, lo.y(i));
  }
  *top = t;
  *bot = bo;
}

}  // namespace internal

// 求点集凸包交叠了哪些栅格 tile（列优先，tile 按 (col,row) 升序输出、已去重）。
// 点集上限 ScreenHull::kMaxPts；网格尺寸非法 / 点集退化 → 返回空。
inline std::vector<TileCoord> ConvexHullCoveredTiles(const TileGrid& grid,
                                                     const std::vector<Vec2d>& pts) {
  std::vector<TileCoord> out;
  CHECK_GT(grid.cell, 0.0) << "cell 必须 > 0";
  CHECK_GE(grid.cols, 1);
  CHECK_GE(grid.rows, 1);
  if (pts.empty() || static_cast<int>(pts.size()) > ScreenHull::kMaxPts) {
    return out;
  }
  ScreenHull hull;
  hull.Build(pts.data(), static_cast<int>(pts.size()));
  if (!hull.valid()) {
    return out;
  }
  const int c0 = internal::ClampIndex(hull.x_min(), grid.cell, grid.cols);
  const int c1 = internal::ClampIndex(hull.x_max(), grid.cell, grid.cols);
  if (c0 > c1) {
    return out;
  }
  int up_hint = 0;
  int lo_hint = 0;
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
    internal::ColumnSpan(hull, a, b, &up_hint, &lo_hint, &top, &bot);
    int r0 = internal::ClampIndex(bot, grid.cell, grid.rows);
    int r1 = internal::ClampIndex(top, grid.cell, grid.rows);
    if (r0 > r1) {
      continue;
    }
    for (int r = r0; r <= r1; ++r) {
      out.push_back(TileCoord{c, r});
    }
  }
  return out;
}

}  // namespace math
}  // namespace geom

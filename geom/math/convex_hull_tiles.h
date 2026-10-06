// ConvexHullTiles — 屏幕空间凸包 → 它交叠的栅格（tile）集合
//
// 用途：给定一个 box 的屏幕投影点（≤ 8 个），快速求出「该凸包覆盖/交叠了哪些 tile」，
// 供 tile culling 用（替代「AABB 矩形」的保守覆盖，给出真轮廓）。
//
// 做法（context 无关、无 GL、可单测）：
//   1. 屏幕空间凸包：点按 x 排序（插入排序）后用**单调链（Andrew）**求上壳 / 下壳——
//      x 升序遍历 + 栈，遇「凹折角」就 pop，直到折角正确。**凸性由这一步保证**
//      （扫描线只假设「顶点按 x 有序、分段线性」，不负责纠正不凸的输入）。
//   2. 扫描线：逐列求该列内凸包的 [底, 顶]（= 列两端插值 ∪ 列内顶点），换算行范围标记。
//      上壳**凹**、下壳**凸** ⇒ 极值可能落在**列内部顶点**（峰 / 谷），必须计入。
//
// 性能取向（context 无关、单线程）：
//   - 壳就是**原始顶点数组**（≤ 8 个 (x,y)，x 严格递增），不套 PWL / 不做 AddSample CHECK；
//   - 每列用**单调游标**在顶点串上走（均摊 O(1)），并**复用**相邻列共享的端点值；
//   - 整团落在单个 tile 内 → 短路直接标一格。
//
// 实现位置：geom/math/，命名空间 geom::math。

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "geom/common/check.h"

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

// 屏幕空间凸包（上/下壳），以**原始顶点数组**保存（x 严格递增）。
class ScreenHull {
 public:
  static constexpr int kMaxPts = 8;

  // 由点集构造（插入排序 + 单调链）。degenerate（有效点 < 2 或所有点同 x）时 valid()==false。
  // Pre-condition: 1 <= n <= kMaxPts。
  void Build(const Vec2d* pts, int n) {
    CHECK(n >= 1 && n <= kMaxPts) << "点数 " << n << " 越界 [1," << kMaxPts << "]";
    valid_ = false;
    un_ = 0;
    ln_ = 0;
    // 插入排序（元素 ≤ 8，比 std::sort 轻）：x 升，同 x 按 y 升。
    Vec2d s[kMaxPts];
    for (int i = 0; i < n; ++i) {
      const Vec2d p = pts[i];
      int j = i;
      while (j > 0 && (s[j - 1].x > p.x || (s[j - 1].x == p.x && s[j - 1].y > p.y))) {
        s[j] = s[j - 1];
        --j;
      }
      s[j] = p;
    }
    int m = n;
    int w = 0;   // 去重（相邻全等）
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
    // 上壳：x 降序，pop 直到左转；再反转成 x 升序。
    Vec2d up[kMaxPts];
    int un = 0;
    for (int i = m - 1; i >= 0; --i) {
      while (un >= 2 && cross(up[un - 2], up[un - 1], s[i]) <= 0) {
        --un;
      }
      up[un++] = s[i];
    }
    for (int i = 0, j = un - 1; i < j; ++i, --j) {
      std::swap(up[i], up[j]);
    }
    // 折叠同 x（竖直边）：上壳取 max y、下壳取 min y —— PWL/扫描线要求 x 严格递增。
    Collapse(up, un, /*upper=*/true, upper_, &un_);
    Collapse(lo, ln, /*upper=*/false, lower_, &ln_);
    x_min_ = s[0].x;
    x_max_ = s[m - 1].x;
    valid_ = true;
  }

  bool valid() const { return valid_; }
  double x_min() const { return x_min_; }
  double x_max() const { return x_max_; }
  // 上/下壳顶点串（x 严格递增，size >= 2 当 valid）。
  const Vec2d* upper() const { return upper_; }
  int upper_size() const { return un_; }
  const Vec2d* lower() const { return lower_; }
  int lower_size() const { return ln_; }

 private:
  static void Collapse(const Vec2d* c, int n, bool upper, Vec2d* out, int* out_n) {
    int k = 0;
    for (int i = 0; i < n; ++i) {
      if (k > 0 && out[k - 1].x == c[i].x) {
        out[k - 1].y = upper ? std::max(out[k - 1].y, c[i].y)
                             : std::min(out[k - 1].y, c[i].y);
      } else {
        out[k++] = c[i];
      }
    }
    *out_n = k;
  }

  bool valid_ = false;
  double x_min_ = 0.0;
  double x_max_ = 0.0;
  Vec2d upper_[kMaxPts];
  int un_ = 0;
  Vec2d lower_[kMaxPts];
  int ln_ = 0;
};

// 折线 c[0..n)（x 严格递增）在 x 处的值（线性插值，越界线性外推）。
inline double ChainValueAt(const Vec2d* c, int n, double x) {
  CHECK_GE(n, 2);
  int i = 0;
  while (i + 2 < n && x > c[i + 1].x) {
    ++i;
  }
  const Vec2d& p = c[i];
  const Vec2d& q = c[i + 1];
  return p.y + (q.y - p.y) * (x - p.x) / (q.x - p.x);
}

namespace internal {

inline int ClampIndex(double v, double cell, int n) {
  CHECK_GT(cell, 0.0);
  // 先算浮点索引再夹断，避免 (v/cell) 在 v 巨大时直接转 int 溢出。
  const double f = std::floor(v / cell);
  if (f <= 0.0) return 0;
  if (f >= static_cast<double>(n - 1)) return n - 1;
  return static_cast<int>(f);
}

// 折线扫描：单调游标，均摊 O(1)。
//   seg   : 当前列左端 a 所在段（[c[seg], c[seg+1]]），只前进。
//   right : 上一列右端插值结果——若本列 a 恰为上一列 b（相邻列共享边界），直接复用。
struct ChainCursor {
  int seg = 0;
  double right = 0.0;
  bool have_right = false;
};

// 折线 c[0..n) 在列 [a, b] 上的极值（take_max=true 取最大 / false 取最小）。
// = max/min(左端插值 ∪ 顶点 c[seg+1..t] ∪ 右端插值)。凸不凸都成立（分段线性的极值在端点或顶点）。
inline double ChainRange(const Vec2d* c, int n, double a, double b, bool take_max,
                         ChainCursor* cur) {
  int s = cur->seg;
  while (s + 2 < n && a > c[s + 1].x) {
    ++s;   // a 单调增 ⇒ 游标只前进
  }
  double left;
  if (cur->have_right) {
    left = cur->right;   // 复用上一列的右端值（列边界共享）
  } else {
    const Vec2d& p = c[s];
    const Vec2d& q = c[s + 1];
    left = p.y + (q.y - p.y) * (a - p.x) / (q.x - p.x);
  }
  double best = take_max ? std::max(left, -1e300) : std::min(left, 1e300);
  int t = s;
  while (t + 2 < n && b > c[t + 1].x) {
    ++t;
  }
  for (int k = s + 1; k <= t; ++k) {
    if (c[k].x >= a && c[k].x <= b) {
      best = take_max ? std::max(best, c[k].y) : std::min(best, c[k].y);
    }
  }
  const Vec2d& p = c[t];
  const Vec2d& q = c[t + 1];
  const double right = p.y + (q.y - p.y) * (b - p.x) / (q.x - p.x);
  best = take_max ? std::max(best, right) : std::min(best, right);
  cur->seg = t;
  cur->right = right;
  cur->have_right = true;
  return best;
}

}  // namespace internal

// 求点集凸包交叠了哪些栅格 tile，**追加**到 *out（不清空）。
// 每个 tile 恰好 append 一次（列内按 row 升序；跨列按 col 升序），**天然无重复**。
// 点集上限 ScreenHull::kMaxPts；网格尺寸非法 / 点集退化 → 不追加。
inline void AppendConvexHullCoveredTiles(const TileGrid& grid,
                                         const std::vector<Vec2d>& pts,
                                         std::vector<TileCoord>* out /*inout*/) {
  CHECK(out != nullptr);
  CHECK_GT(grid.cell, 0.0) << "cell 必须 > 0";
  CHECK_GE(grid.cols, 1);
  CHECK_GE(grid.rows, 1);
  // 点数上限是硬约束（box = 8）：超了是调用方错误，crash 早暴露（不静默吞）。
  CHECK_LE(pts.size(), static_cast<size_t>(ScreenHull::kMaxPts))
      << "点数 " << pts.size() << " 超过上限 " << ScreenHull::kMaxPts;
  if (pts.empty()) {
    return;   // 无点 → 无覆盖（合法退化）
  }
  ScreenHull hull;
  hull.Build(pts.data(), static_cast<int>(pts.size()));
  if (!hull.valid()) {
    return;
  }
  const double cell = grid.cell;
  const int c0 = internal::ClampIndex(hull.x_min(), cell, grid.cols);
  const int c1 = internal::ClampIndex(hull.x_max(), cell, grid.cols);
  if (c0 > c1) {
    return;
  }
  // 短路：整团（含顶点 y 跨度）落在单个 tile 内 → 直接标一格。
  // （壳的 y 极值在顶点上 ⇒ 顶点 y 跨度 = 凸包 y 跨度。）
  if (c0 == c1) {
    double ymin = 1e300;
    double ymax = -1e300;
    const Vec2d* up = hull.upper();
    const Vec2d* lo = hull.lower();
    for (int i = 0; i < hull.upper_size(); ++i) {
      ymin = std::min(ymin, up[i].y);
      ymax = std::max(ymax, up[i].y);
    }
    for (int i = 0; i < hull.lower_size(); ++i) {
      ymin = std::min(ymin, lo[i].y);
      ymax = std::max(ymax, lo[i].y);
    }
    const int r0 = internal::ClampIndex(ymin, cell, grid.rows);
    const int r1 = internal::ClampIndex(ymax, cell, grid.rows);
    if (r0 == r1) {
      out->push_back(TileCoord{c0, r0});
      return;
    }
  }
  internal::ChainCursor up_cur;
  internal::ChainCursor lo_cur;
  const Vec2d* up = hull.upper();
  const int up_n = hull.upper_size();
  const Vec2d* lo = hull.lower();
  const int lo_n = hull.lower_size();
  const double hx0 = hull.x_min();
  const double hx1 = hull.x_max();
  for (int c = c0; c <= c1; ++c) {
    // 首/末列的列区间会超出凸包的 x 范围；**取交**，否则插值会外推出界、多标 tile。
    const double a = std::max(static_cast<double>(c) * cell, hx0);
    const double b = std::min(static_cast<double>(c + 1) * cell, hx1);
    if (a > b) {
      continue;
    }
    const double top = internal::ChainRange(up, up_n, a, b, /*take_max=*/true, &up_cur);
    const double bot = internal::ChainRange(lo, lo_n, a, b, /*take_max=*/false, &lo_cur);
    const int r0 = internal::ClampIndex(bot, cell, grid.rows);
    const int r1 = internal::ClampIndex(top, cell, grid.rows);
    for (int r = r0; r <= r1; ++r) {
      out->push_back(TileCoord{c, r});
    }
    // 注：相邻列 a == 上一列 b，ChainCursor.right 已缓存该端点值供下一列复用。
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

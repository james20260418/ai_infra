// tools/jpov/tools/util/nearest_point_matcher_3d.h — 「点到最近折线点」查询（稀疏栅格加速）
//
// 问题：给一堆折线（例如植被骨架：几千条、每条十几个采样点），对任意查询点找
// 「折线上离它最近的那个点」，并回答它属于哪条折线、哪一段、段内参数 t。
// 朴素做法是遍历全部线段（O(线段数)）；本容器把线段装进**稀疏**均匀栅格
// （只有被线段碰到的格子才存在），查询从查询点附近往外做 best-first，典型 O(1)。
//
// 为什么是「点到线段」而不是「点到采样点」：采样点之间是直线段，最近点常常落在
// 段中间。只查最近采样点会有系统性偏差（最坏半段长）——把它当成「这个点是否在
// 某条折线上」的判据时，这个偏差会直接变成错误的归属（例如把子枝挂到错误的父枝）。
//
// === 正确性（改这个文件前先读完）===============================================
//
// 1) 装箱（Build）：把每条线段按长度切成 ≤ cell_size 的小段，每个小段把自己**轴对齐
//    包围盒**覆盖到的格子全部登记（桶里存线段下标）。于是「线段上任意一点所在的格子」
//    一定登记了这条线段 —— 召回是精确的，不是概率的。
//    只按两个端点所在格登记是**错的**：斜穿多格的长线段会漏掉中间那些格。
//
// 2) 查询 = 在格子上的 best-first（Dijkstra 式）：
//    每个格子算一个**下界** LB(cell) = 「查询点到该格子立方体的距离」——
//    格子里任何一点都不会比 LB 更近。用最小堆按 LB 从小到大弹格子，弹到
//    LB ≥ 当前最优距离就停：剩下的格子都更远，不可能有改进。
//      * 这是**精确**的，不依赖任何背景假设；也不存在「环形扩张半径算少了」的坑。
//      * 只在包围盒覆盖的格子范围内扩展（所有线段都在盒内，盒外的格子必然为空），
//        所以远在盒外的查询点不会白扫盒子外的格子。
//      * 下界用平方距离比较，省掉 sqrt。
//
// 2b) 远查询的降级：均匀栅格天生不适合「查询点离数据很远」——那时半径 best 的球
//    扫过盒内的格数可以到百万级（格子越小越惨）。所以访问格子数超过 kMaxVisitedCells
//    就直接降级成「全量扫线段」：最差回到朴素实现的复杂度，不会比它更慢，也不会错。
//    正常使用（查询点落在数据附近）根本走不到这个分支。
//
//    （更早的版本用「环形扩张 + (r−1)·cell 上界」并先把查询点夹到包围盒上；
//      后者是**错的**：夹回只在「查询点与盒子只差一个轴」时保持距离排序，多个轴
//      同时在外时 (q_i−s_i) 的交叉项不可忽略，排序会变 —— 单测里 cell_size 变小
//      就露馅。别改回去。）
//
// 3) 退化情形：折线只有一个点 → 记成一条 a==b 的退化线段，返回 segment = -1、t = 0；
//    相邻采样点重合 → 按零长线段处理（t 固定为 0，不会出现 0/0）。
//
// === 其它取舍 =================================================================
//   * 稀疏存储复用 geom/common 的 GridMap（哈希表），坐标→格索引用 geom/common/grid。
//     坐标范围受 grid 夹断限制（|坐标 / cell_size| 要能放进 int），远超米制场景。
//   * 与 geom/3d/triangle_matcher_3d.h 同一套思路（体素 + 候选桶）。区别是那边用调用方
//     给的 local_distance 离线预计算每格候选表、查询严格 O(1)；这边要支持**无半径的
//     最近邻**，所以用 best-first，不需要调用方给半径。若将来引擎侧也要用，可整体
//     挪到 geom/3d 与它作伴。
//   * 构建后只读，可多线程并发 Query；构建期间不行。
//
// 单测策略：与 brute force（遍历全部线段取最小）逐点对照，见 nearest_point_matcher_3d_test.cc。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "geom/common/check.h"
#include "geom/common/grid.h"
#include "geom/common/grid_map.h"
#include "geom/common/vec.h"

namespace jpov {
namespace util {

// 点到「一组折线」的最近点查询容器。
//
// 典型用法：
//   NearestPointMatcher3d<double> matcher(polylines, /*cell_size=*/0.25);
//   std::optional<Match> m = matcher.Query(p);
//   if (m.has_value() && m->distance < 0.01) { /* p 落在这条折线上 */ }
//
//   // 「这根枝的父枝是谁」：必须排除自己那条折线（否则距离恒为 0，命中自己）
//   std::optional<Match> parent = matcher.Query(branch_start, /*exclude_polyline=*/self);
template <typename T>
class NearestPointMatcher3d {
 public:
  using Vec3 = geom::Vec3<T>;
  using Grid = geom::Grid<3, int, T>;
  using CellIndex = typename Grid::Indice;

  // 一次查询的结果。
  struct Match {
    int polyline = -1;                     // 折线下标（= 构造时传入的数组下标）
    int segment = -1;                      // 折线内线段下标；折线只有一个点时是 -1
    T t = T(0);                            // 线段内参数 [0,1]（退化线段恒为 0）
    Vec3 point = Vec3(T(0), T(0), T(0));   // 折线上的最近点
    T distance = T(0);                     // |query − point|
  };

  // Pre-condition（不满足即 LOG(FATAL)）：
  //   - cell_size > 0：格子边长（与坐标同单位）。经验值取「线段平均长度」量级：
  //     太小则绝大多数格为空、太大则一格塞很多线段 —— 两者都只是变慢，不影响正确性；
  //   - polylines 非空、且每条折线至少有 1 个点：对着空气建索引是编程错误。
  //
  // polylines 按值保存（容器持有副本）；返回值里的下标即这份副本的下标。
  NearestPointMatcher3d(std::vector<std::vector<Vec3>> polylines, T cell_size);

  // 返回离 query 最近的折线点。容器非空 ⇒ 一定有结果（除非被 exclude_polyline 排完）。
  //
  // exclude_polyline >= 0 时跳过该折线上的所有线段 —— 「查这根枝的父枝」就必须这么用：
  // 否则查询点（子枝起点）到它自己那条折线的距离恒为 0，永远命中自己。
  //
  // Pre-condition: query 坐标必须是有限值（NaN 会触发 grid 的 CHECK）；
  //                exclude_polyline 要么是 -1，要么是合法下标。
  std::optional<Match> Query(const Vec3& query, int exclude_polyline = -1) const;

  int num_polylines() const {
    return static_cast<int>(polylines_.size());
  }
  int num_segments() const {
    return static_cast<int>(segments_.size());
  }
  // 实际占用（非空）的格子数 —— 用来验证「稀疏」这件事。
  std::size_t num_cells() const {
    return buckets_.size();
  }
  T cell_size() const {
    return cell_size_;
  }

 private:
  struct Segment {
    Vec3 a;
    Vec3 b;
    int polyline = -1;
    int segment = -1;  // 折线只有一个点时是 -1
  };

  // best-first 的待访问格子（按下界从小到大弹）。
  struct PendingCell {
    T lower_bound_sqr = T(0);
    CellIndex cell = CellIndex{ 0, 0, 0 };
  };
  struct PendingCellGreater {
    bool operator()(const PendingCell& lhs, const PendingCell& rhs) const {
      return lhs.lower_bound_sqr > rhs.lower_bound_sqr;
    }
  };

  CellIndex CellOf(const Vec3& point) const;
  // 查询点到该格子立方体的**平方**距离（格子里任何一点都不会比它更近）。
  T LowerBoundSqrToCell(const Vec3& query, const CellIndex& cell) const;
  bool IsCellInBounds(const CellIndex& cell) const;
  // 线段到查询点的**平方**距离；t 写入线段内参数 [0,1]。
  T EvaluateSegmentSqr(int segment_id, const Vec3& query, T* t /*output*/) const;
  void Build();
  void InsertSegment(int segment_id);

  // 访问格子超过这个数就降级成全量扫线段（见文件头 2b）。
  // 取值依据：正常查询（数据附近）只会弹几个到几十个格；而“半径 best 的球扫过大量
  // 空格子”那种远查询，多扫几百格的收益远不如直接全量扫一遍线段。
  static constexpr int kMaxVisitedCells = 512;

  T cell_size_;
  CellIndex cell_min_ = CellIndex{ 0, 0, 0 };  // 包围盒覆盖的格子范围（含两端）
  CellIndex cell_max_ = CellIndex{ 0, 0, 0 };
  std::vector<std::vector<Vec3>> polylines_;
  std::vector<Segment> segments_;
  geom::GridMap<std::vector<int>, Grid> buckets_;
};

// ============================ Implementation ============================

template <typename T>
NearestPointMatcher3d<T>::NearestPointMatcher3d(std::vector<std::vector<Vec3>> polylines,
                                                T cell_size)
    : cell_size_(cell_size), polylines_(std::move(polylines)) {
  CHECK_GT(cell_size_, T(0)) << "NearestPointMatcher3d: cell_size 必须 > 0。";
  CHECK(!polylines_.empty()) << "NearestPointMatcher3d: polylines 不能为空。";
  for (const std::vector<Vec3>& polyline : polylines_) {
    CHECK(!polyline.empty()) << "NearestPointMatcher3d: 每条折线至少要有 1 个点。";
  }
  Build();
}

template <typename T>
typename NearestPointMatcher3d<T>::CellIndex NearestPointMatcher3d<T>::CellOf(
    const Vec3& point) const {
  CellIndex index;
  index[0] = Grid::ValueToGridIndex(point.x() / cell_size_);
  index[1] = Grid::ValueToGridIndex(point.y() / cell_size_);
  index[2] = Grid::ValueToGridIndex(point.z() / cell_size_);
  return index;
}

template <typename T>
T NearestPointMatcher3d<T>::LowerBoundSqrToCell(const Vec3& query,
                                                const CellIndex& cell) const {
  T sum = T(0);
  for (int axis = 0; axis < 3; ++axis) {
    const T lo = static_cast<T>(cell[axis]) * cell_size_;
    const T hi = lo + cell_size_;
    const T value = query[axis];
    if (value < lo) {
      sum += (lo - value) * (lo - value);
    } else if (value > hi) {
      sum += (value - hi) * (value - hi);
    }
  }
  return sum;
}

template <typename T>
bool NearestPointMatcher3d<T>::IsCellInBounds(const CellIndex& cell) const {
  for (int axis = 0; axis < 3; ++axis) {
    if (cell[axis] < cell_min_[axis] || cell[axis] > cell_max_[axis]) {
      return false;
    }
  }
  return true;
}

template <typename T>
void NearestPointMatcher3d<T>::Build() {
  // 展平成线段表，同时统计包围盒（用于限定 best-first 的搜索范围）。
  Vec3 bounds_min = polylines_[0][0];
  Vec3 bounds_max = polylines_[0][0];
  for (int i = 0; i < static_cast<int>(polylines_.size()); ++i) {
    const std::vector<Vec3>& polyline = polylines_[i];
    for (const Vec3& point : polyline) {
      bounds_min = Vec3(std::min(bounds_min.x(), point.x()), std::min(bounds_min.y(), point.y()),
                        std::min(bounds_min.z(), point.z()));
      bounds_max = Vec3(std::max(bounds_max.x(), point.x()), std::max(bounds_max.y(), point.y()),
                        std::max(bounds_max.z(), point.z()));
    }
    if (polyline.size() == 1) {
      Segment segment;
      segment.a = polyline[0];
      segment.b = polyline[0];
      segment.polyline = i;
      segment.segment = -1;
      segments_.push_back(segment);
      continue;
    }
    for (int k = 0; k + 1 < static_cast<int>(polyline.size()); ++k) {
      Segment segment;
      segment.a = polyline[k];
      segment.b = polyline[k + 1];
      segment.polyline = i;
      segment.segment = k;
      segments_.push_back(segment);
    }
  }
  cell_min_ = CellOf(bounds_min);
  cell_max_ = CellOf(bounds_max);

  for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
    InsertSegment(i);
  }
}

template <typename T>
void NearestPointMatcher3d<T>::InsertSegment(int segment_id) {
  const Segment& segment = segments_[segment_id];
  const Vec3 delta = segment.b - segment.a;
  // 按 ≤ cell_size 切段：既保证「线段上的点所在格一定被登记」，又让每个小段的
  // AABB 只覆盖常数个格子（长线段不会一次登记进几万个格子）。
  const int pieces = std::max(1, static_cast<int>(std::ceil(delta.Norm() / cell_size_)));
  for (int k = 0; k < pieces; ++k) {
    const T t0 = static_cast<T>(k) / static_cast<T>(pieces);
    const T t1 = static_cast<T>(k + 1) / static_cast<T>(pieces);
    const Vec3 p0 = segment.a + delta * t0;
    const Vec3 p1 = segment.a + delta * t1;
    const CellIndex lo = CellOf(Vec3(std::min(p0.x(), p1.x()), std::min(p0.y(), p1.y()),
                                     std::min(p0.z(), p1.z())));
    const CellIndex hi = CellOf(Vec3(std::max(p0.x(), p1.x()), std::max(p0.y(), p1.y()),
                                     std::max(p0.z(), p1.z())));
    for (int ix = lo[0]; ix <= hi[0]; ++ix) {
      for (int iy = lo[1]; iy <= hi[1]; ++iy) {
        for (int iz = lo[2]; iz <= hi[2]; ++iz) {
          // 相邻小段会在边界格重复登记同一条线段：多算一次距离，不影响结果。
          buckets_[CellIndex{ ix, iy, iz }].push_back(segment_id);
        }
      }
    }
  }
}

template <typename T>
T NearestPointMatcher3d<T>::EvaluateSegmentSqr(int segment_id, const Vec3& query,
                                               T* t /*output*/) const {
  const Segment& segment = segments_[segment_id];
  const Vec3 ab = segment.b - segment.a;
  const T length_sqr = ab.Sqr();
  T local_t = T(0);
  if (length_sqr > T(0)) {
    const Vec3 ap = query - segment.a;
    const T dot = ap.x() * ab.x() + ap.y() * ab.y() + ap.z() * ab.z();
    local_t = std::clamp(dot / length_sqr, T(0), T(1));
  }
  *t = local_t;
  return (query - (segment.a + ab * local_t)).Sqr();
}

template <typename T>
std::optional<typename NearestPointMatcher3d<T>::Match> NearestPointMatcher3d<T>::Query(
    const Vec3& query, int exclude_polyline) const {
  CHECK(std::isfinite(query.x()) && std::isfinite(query.y()) && std::isfinite(query.z()))
      << "NearestPointMatcher3d::Query: 查询点必须是有限值。";
  CHECK(exclude_polyline >= -1 && exclude_polyline < static_cast<int>(polylines_.size()))
      << "NearestPointMatcher3d::Query: exclude_polyline 越界。";

  T best_sqr = std::numeric_limits<T>::infinity();
  int best_id = -1;
  bool fallback_to_brute_force = false;
  int popped_cells = 0;

  // 格子状态：1 = 已入堆，2 = 已弹出。入堆时就标记，避免同一格被 26 个邻居反复压堆。
  geom::GridMap<uint8_t, Grid> state;
  std::priority_queue<PendingCell, std::vector<PendingCell>, PendingCellGreater> pending;

  // 起点：查询点所在格（若在包围盒外，先夹到盒内最近的那个格；盒外没有线段）。
  CellIndex start = CellOf(query);
  for (int axis = 0; axis < 3; ++axis) {
    start[axis] = std::clamp(start[axis], cell_min_[axis], cell_max_[axis]);
  }
  PendingCell first;
  first.cell = start;
  first.lower_bound_sqr = LowerBoundSqrToCell(query, start);
  state[start] = 1;
  pending.push(first);

  while (!pending.empty()) {
    const PendingCell top = pending.top();
    // 剩下的格子都比当前最优更远（下界单调），可以收工。
    if (best_id >= 0 && top.lower_bound_sqr >= best_sqr) {
      break;
    }
    pending.pop();
    const typename geom::GridMap<uint8_t, Grid>::const_iterator state_it = state.find(top.cell);
    if (state_it != state.end() && state_it->second == 2) {
      continue;
    }
    state[top.cell] = 2;
    ++popped_cells;
    // 远查询时半径 best 的球会扫过太多空格子：降级成全量扫（见文件头 2b）。
    if (popped_cells > kMaxVisitedCells) {
      fallback_to_brute_force = true;
      break;
    }

    const geom::GridMap<std::vector<int>, Grid>& buckets = buckets_;
    const typename geom::GridMap<std::vector<int>, Grid>::const_iterator it =
        buckets.find(top.cell);
    if (it != buckets.end()) {
      for (const int segment_id : it->second) {
        if (segments_[segment_id].polyline == exclude_polyline) {
          continue;
        }
        T t = T(0);
        const T distance_sqr = EvaluateSegmentSqr(segment_id, query, &t);
        // tie-break 用线段下标：等距时结果与遍历顺序无关（堆里同下界格子的顺序不保证）。
        if (distance_sqr < best_sqr ||
            (distance_sqr == best_sqr && segment_id < best_id)) {
          best_sqr = distance_sqr;
          best_id = segment_id;
        }
      }
    }

    // 把 26 个邻居入堆（只限包围盒覆盖的格子）。
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
          if (dx == 0 && dy == 0 && dz == 0) {
            continue;
          }
          const CellIndex neighbor = { top.cell[0] + dx, top.cell[1] + dy,
                                       top.cell[2] + dz };
          if (!IsCellInBounds(neighbor) || state.find(neighbor) != state.end()) {
            continue;
          }
          PendingCell next;
          next.cell = neighbor;
          next.lower_bound_sqr = LowerBoundSqrToCell(query, neighbor);
          state[neighbor] = 1;
          pending.push(next);
        }
      }
    }
  }

  if (fallback_to_brute_force) {
    best_sqr = std::numeric_limits<T>::infinity();
    best_id = -1;
    for (int segment_id = 0; segment_id < static_cast<int>(segments_.size()); ++segment_id) {
      if (segments_[segment_id].polyline == exclude_polyline) {
        continue;
      }
      T t = T(0);
      const T distance_sqr = EvaluateSegmentSqr(segment_id, query, &t);
      if (distance_sqr < best_sqr || (distance_sqr == best_sqr && segment_id < best_id)) {
        best_sqr = distance_sqr;
        best_id = segment_id;
      }
    }
  }

  if (best_id < 0) {
    return std::nullopt;  // 只有坐标被 grid 夹断到失真时才可能走到这里
  }
  const Segment& segment = segments_[best_id];
  T t = T(0);
  EvaluateSegmentSqr(best_id, query, &t);
  Match match;
  match.polyline = segment.polyline;
  match.segment = segment.segment;
  match.t = t;
  match.point = segment.a + (segment.b - segment.a) * t;
  match.distance = (match.point - query).Norm();
  return match;
}

}  // namespace util
}  // namespace jpov

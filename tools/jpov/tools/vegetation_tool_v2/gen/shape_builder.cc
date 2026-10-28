#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_builder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/tools/util/nearest_point_matcher_3d.h"

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

// 锚点离枝的容差（米）：叶柄允许插进管壁，但不许悬空。与 Python 版一致。
constexpr double kAnchorSlack = 0.01;

// 判定「样条是父枝的延续而不是分叉」的附着参数阈值（与 Python 版一致）。
constexpr double kContinuationAttach = 0.98;

// 叶卡高度小于这个值就当退化卡丢弃（米）。
constexpr double kMinCardHeight = 1e-6;

// ----------------------------- 向量小工具 -----------------------------

Vec3d Add(const Vec3d& a, const Vec3d& b) {
  return Vec3d{ a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3d Sub(const Vec3d& a, const Vec3d& b) {
  return Vec3d{ a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3d Mul(const Vec3d& a, double scale) {
  return Vec3d{ a.x * scale, a.y * scale, a.z * scale };
}

double Dot(const Vec3d& a, const Vec3d& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d Cross(const Vec3d& a, const Vec3d& b) {
  return Vec3d{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

double LenSqr(const Vec3d& a) {
  return Dot(a, a);
}

double Len(const Vec3d& a) {
  return std::sqrt(LenSqr(a));
}

// 归一化；零向量退回 +Y（与 Python 的 norm3 一致）。
Vec3d Normalize(const Vec3d& a) {
  const double length = Len(a);
  if (length < 1e-12) {
    return Vec3d{ 0.0, 1.0, 0.0 };
  }
  return Vec3d{ a.x / length, a.y / length, a.z / length };
}

// Blender(+Z up) → 资产格式(+Y up)。
Vec3d ToUp(const Vec3d& v) {
  return Vec3d{ v.x, v.z, -v.y };
}

geom::Vec3<double> ToGeom(const Vec3d& v) {
  return geom::Vec3<double>(v.x, v.y, v.z);
}

// ----------------------------- 采样 -----------------------------

struct SampledPolyline {
  std::vector<Vec3d> points;  // Blender 坐标系、世界空间
  std::vector<double> radii;  // 米
};

Vec3d EvaluateBezier(const Vec3d& p0, const Vec3d& p1, const Vec3d& p2, const Vec3d& p3,
                     double t) {
  const double mt = 1.0 - t;
  const double w0 = mt * mt * mt;
  const double w1 = 3.0 * mt * mt * t;
  const double w2 = 3.0 * mt * t * t;
  const double w3 = t * t * t;
  return Vec3d{ p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3,
                p0.z * w0 + p1.z * w1 + p2.z * w2 + p3.z * w3 };
}

SampledPolyline SampleSpline(const DumpSpline& spline, double bevel_depth, uint32_t per_seg) {
  SampledPolyline sampled;
  if (spline.is_bezier) {
    for (std::size_t i = 0; i + 1 < spline.points.size(); ++i) {
      const DumpSplinePoint& p0 = spline.points[i];
      const DumpSplinePoint& p3 = spline.points[i + 1];
      for (uint32_t k = 0; k < per_seg; ++k) {
        const double t = static_cast<double>(k) / static_cast<double>(per_seg);
        sampled.points.push_back(
            EvaluateBezier(p0.co, p0.handle_right, p3.handle_left, p3.co, t));
        sampled.radii.push_back((1.0 - t) * p0.radius + t * p3.radius);
      }
    }
    sampled.points.push_back(spline.points.back().co);
    sampled.radii.push_back(spline.points.back().radius);
  } else {
    for (const DumpSplinePoint& point : spline.points) {
      sampled.points.push_back(point.co);
      sampled.radii.push_back(point.radius);
    }
  }
  if (sampled.radii.size() != sampled.points.size()) {
    sampled.radii.assign(sampled.points.size(), 1.0);
  }
  for (double& radius : sampled.radii) {
    // 半径下限 1e-4 m：Sapling 的末梢 radius 可能正好是 0，而资产格式要求 radius > 0
    // （Validate 会拒收）。Python 版在写文件时才夹，这里提前夹，效果相同。
    radius = std::max(1e-4, std::max(0.0, radius * bevel_depth));
  }
  return sampled;
}

// ----------------------------- 层级（父枝 / 层数） -----------------------------

struct Hierarchy {
  int root = 0;
  std::vector<int> parent;    // -1 = 根
  std::vector<int> level;
  std::vector<double> attach;  // 附着点在父枝上的归一化参数（判定「延续」用）
};

// 数据整体的包围盒对角线 —— 用来给栅格挑一个合理的格子边长。
double Diagonal(const std::vector<SampledPolyline>& polys) {
  Vec3d low = polys[0].points[0];
  Vec3d high = polys[0].points[0];
  for (const SampledPolyline& polyline : polys) {
    for (const Vec3d& point : polyline.points) {
      low = Vec3d{ std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z) };
      high = Vec3d{ std::max(high.x, point.x), std::max(high.y, point.y),
                    std::max(high.z, point.z) };
    }
  }
  return Len(Sub(high, low));
}

Hierarchy BuildHierarchy(const std::vector<SampledPolyline>& polys) {
  const int count = static_cast<int>(polys.size());
  Hierarchy hierarchy;
  hierarchy.parent.assign(count, -1);
  hierarchy.level.assign(count, 0);
  hierarchy.attach.assign(count, 0.0);

  // 根 = 起点最低的那条（z 取到 1e-4 再比，平手时取起点离原点更近的）—— 与 Python 一致。
  int root = 0;
  double best_z = 0.0;
  double best_length = 0.0;
  for (int i = 0; i < count; ++i) {
    const Vec3d& start = polys[i].points[0];
    const double z = std::round(start.z * 1e4) / 1e4;
    const double length = Len(start);
    if (i == 0 || z < best_z || (z == best_z && length < best_length)) {
      root = i;
      best_z = z;
      best_length = length;
    }
  }
  hierarchy.root = root;

  // 每条折线拿自己的起点去问「最近的其他折线」= 父枝。
  std::vector<std::vector<geom::Vec3<double>>> polylines;
  polylines.reserve(static_cast<std::size_t>(count));
  for (const SampledPolyline& polyline : polys) {
    std::vector<geom::Vec3<double>> converted;
    converted.reserve(polyline.points.size());
    for (const Vec3d& point : polyline.points) {
      converted.push_back(ToGeom(point));
    }
    polylines.push_back(std::move(converted));
  }
  const double cell_size = std::max(Diagonal(polys) / 64.0, 1e-6);
  const jpov::util::NearestPointMatcher3d<double> matcher(std::move(polylines), cell_size);

  for (int i = 0; i < count; ++i) {
    if (i == root) {
      continue;  // 根没有父枝，也不该去问（问出来的只可能是"最近的那条枝"）
    }
    const std::optional<jpov::util::NearestPointMatcher3d<double>::Match> match =
        matcher.Query(ToGeom(polys[i].points[0]), /*exclude_polyline=*/i);
    if (!match.has_value()) {
      continue;
    }
    hierarchy.parent[i] = match->polyline;
    const int segment_count = static_cast<int>(polys[match->polyline].points.size()) - 1;
    hierarchy.attach[i] =
        (static_cast<double>(match->segment) + match->t) / std::max(1, segment_count);
  }

  // 兜底：破环。
  // 父枝是用「起点最近的折线」判的，采样一稀（低配 per_seg=1~2）时，某根枝的起点
  // 可能离它自己**后代**的近路折线更近 ⇒ 父子关系成环（A 的父是 B、B 的父是 A），
  // 拓扑序写不出来，整棵树直接失败（报「枝 N 的父枝排在它后面」）。
  // 处理：把环上的枝重新问一次「最近的其他折线」（**排除自己**）；命中的若还是
  // 当前父枝（环没解开）或压根没有其他折线，就把这根枝挂到根枝上。
  //
  // ⚠️ 排除的必须是**自己**而不是当前父枝：查询点就是这根枝自己的起点，到自己的
  // 距离恒为 0；不排除自己就永远命中自己，这个「换一个父亲」的分支等于没写。
  for (int pass = 0; pass < 4; ++pass) {
    bool repaired = false;
    for (int i = 0; i < count; ++i) {
      // 沿父链往上走，看会不会回到自己。
      int current = i;
      int steps = 0;
      while (current >= 0 && steps <= count && !(current == i && steps > 0)) {
        current = hierarchy.parent[static_cast<std::size_t>(current)];
        ++steps;
      }
      if (!(current == i && steps > 0)) {
        continue;  // 没有环
      }
      const int parent = hierarchy.parent[static_cast<std::size_t>(i)];
      // 最近的其他折线（排除自己）；它还是当前父枝就说明环没解开，走下面的兜底。
      const std::optional<jpov::util::NearestPointMatcher3d<double>::Match> retry =
          matcher.Query(ToGeom(polys[i].points[0]), i);
      if (retry.has_value() && retry->polyline >= 0 && retry->polyline != parent &&
          retry->polyline != i) {
        hierarchy.parent[static_cast<std::size_t>(i)] = retry->polyline;
        const int segment_count = static_cast<int>(polys[retry->polyline].points.size()) - 1;
        hierarchy.attach[static_cast<std::size_t>(i)] =
            (static_cast<double>(retry->segment) + retry->t) / std::max(1, segment_count);
      } else {
        hierarchy.parent[static_cast<std::size_t>(i)] = root;  // 最后兜底：挂到根枝
        hierarchy.attach[static_cast<std::size_t>(i)] = 1.0;
      }
      repaired = true;
    }
    if (!repaired) {
      break;  // 已经没有环了（正常情况下第一轮就清干净）
    }
  }

  // 层数：BFS 从根往下走；父枝是「延续」（附着点几乎在末端）时层数不变，分叉才 +1。
  std::vector<std::vector<int>> children(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (i != root) {
      children[static_cast<std::size_t>(std::max(hierarchy.parent[i], 0))].push_back(i);
    }
  }
  std::deque<int> queue;
  queue.push_back(root);
  std::vector<bool> visited(static_cast<std::size_t>(count), false);
  visited[static_cast<std::size_t>(root)] = true;
  while (!queue.empty()) {
    const int current = queue.front();
    queue.pop_front();
    for (const int child : children[static_cast<std::size_t>(current)]) {
      if (visited[static_cast<std::size_t>(child)]) {
        continue;
      }
      visited[static_cast<std::size_t>(child)] = true;
      const int parent = hierarchy.parent[child];
      hierarchy.level[child] =
          hierarchy.attach[child] > kContinuationAttach
              ? hierarchy.level[static_cast<std::size_t>(parent)]
              : hierarchy.level[static_cast<std::size_t>(parent)] + 1;
      queue.push_back(child);
    }
  }
  return hierarchy;
}

// 拓扑序：父枝必须先于子枝。BFS 走完后再把不可达的沿父链补进来（环则打断）。
std::vector<int> TopologicalOrder(const std::vector<int>& parent) {
  const int count = static_cast<int>(parent.size());
  std::vector<std::vector<int>> children(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (parent[i] >= 0) {
      children[static_cast<std::size_t>(parent[i])].push_back(i);
    }
  }
  std::vector<int> order;
  std::vector<bool> placed(static_cast<std::size_t>(count), false);
  std::deque<int> queue;
  for (int i = 0; i < count; ++i) {
    if (parent[i] < 0) {
      queue.push_back(i);
    }
  }
  while (!queue.empty()) {
    const int current = queue.front();
    queue.pop_front();
    if (placed[static_cast<std::size_t>(current)]) {
      continue;
    }
    placed[static_cast<std::size_t>(current)] = true;
    order.push_back(current);
    for (const int child : children[static_cast<std::size_t>(current)]) {
      queue.push_back(child);
    }
  }
  // 兜底：不可达的样条（数据坏了 / 成环）也要导出，但先把祖先插进来。
  for (int i = 0; i < count; ++i) {
    if (placed[static_cast<std::size_t>(i)]) {
      continue;
    }
    std::vector<int> chain;
    std::vector<bool> in_chain(static_cast<std::size_t>(count), false);
    int current = i;
    while (current >= 0 && !placed[static_cast<std::size_t>(current)]) {
      if (in_chain[static_cast<std::size_t>(current)]) {
        break;  // 成环：到此为止，剩下的按当前顺序追加
      }
      in_chain[static_cast<std::size_t>(current)] = true;
      chain.push_back(current);
      current = parent[static_cast<std::size_t>(current)];
    }
    for (std::vector<int>::reverse_iterator it = chain.rbegin(); it != chain.rend(); ++it) {
      if (!placed[static_cast<std::size_t>(*it)]) {
        placed[static_cast<std::size_t>(*it)] = true;
        order.push_back(*it);
      }
    }
  }
  return order;
}

// ----------------------------- 叶卡 -----------------------------

struct BuiltCard {
  Vec3d anchor;
  Vec3d up;
  Vec3d normal;
  double height = 0.0;
  int branch = -1;
  double s = 0.0;
  bool mirrored = false;
};

// 一张卡的四个角（glTF 空间）。
struct CardCorners {
  Vec3d corner[4];
  bool has_uv = true;
  double u[4] = { 0.0, 0.0, 0.0, 0.0 };
  double v[4] = { 0.0, 0.0, 0.0, 0.0 };
};

// 点到折线的距离；顺带返回该处半径（用于 anchor 合同校验）。
double DistanceToPolyline(const Vec3d& point, const SampledPolyline& polyline, double* radius) {
  double best = std::numeric_limits<double>::infinity();
  double best_radius = polyline.radii.empty() ? 0.0 : polyline.radii[0];
  for (std::size_t k = 0; k + 1 < polyline.points.size(); ++k) {
    const Vec3d a = polyline.points[k];
    const Vec3d ab = Sub(polyline.points[k + 1], a);
    const double length_sqr = LenSqr(ab);
    double t = 0.0;
    if (length_sqr > 0.0) {
      t = std::clamp(Dot(Sub(point, a), ab) / length_sqr, 0.0, 1.0);
    }
    const double distance = Len(Sub(point, Add(a, Mul(ab, t))));
    if (distance < best) {
      best = distance;
      const double r0 = polyline.radii[std::min(k, polyline.radii.size() - 1)];
      const double r1 = polyline.radii[std::min(k + 1, polyline.radii.size() - 1)];
      best_radius = r0 + (r1 - r0) * t;
    }
  }
  *radius = best_radius;
  return best;
}

// ----------------------------- 枝条噪声 -----------------------------
// 对应 generate.proto 的 ShapeNoise：尺寸抖动 + 纵向/横向弯度。
//
// 全部在资产坐标系（+Y up）里施加，按拓扑序处理（父枝一定先于子枝）：
//   1) 把枝根挪到「已经噪声过的父枝」上 —— 子枝必须跟着父枝走，否则父枝一弯，
//      子枝的根就悬在半空；
//   2) 以枝根为中心整体缩放：长度与半径乘同一个因子（整体变大/变小，不是被拉长）；
//   3) 沿枝加两个方向的弯曲位移，位移在两端恒为 0（枝根不动 ⇒ 接缝不裂；
//      枝梢不动 ⇒ 末梢叶子不飘）。
// 结果直接写回折线；下游取叶卡 anchor、生成管子时读到的就是歪好的几何。
constexpr double kPi = 3.14159265358979323846;

// 一个采样点的姿态旋转（叶卡跟着枝走用）：把「噪声前的切线」转到「噪声后的切线」。
struct SampleRotation {
  Vec3d axis{ 0.0, 1.0, 0.0 };
  double angle = 0.0;  // 弧度；0 = 不变
};

// splitmix64：稳定、与平台无关。同 seed + 同枝 + 同用途 ⇒ 同一个数。
uint64_t MixHash(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// [-1, 1) 的确定性随机数。salt 区分用途（尺寸因子 / 弯曲相位……）。
double UnitFromHash(int64_t seed, int branch, uint64_t salt) {
  const uint64_t mixed =
      MixHash(static_cast<uint64_t>(seed) ^ MixHash(static_cast<uint64_t>(branch + 1) * salt));
  return static_cast<double>(mixed >> 11) * (1.0 / 4503599627370496.0) - 1.0;
}

// 取某一级的值；数组短了 = 该级不启用；负值按 0 处理（语义上只有 0..1）。
double LevelValue(const std::vector<float>& values, std::size_t level) {
  if (level >= values.size()) {
    return 0.0;
  }
  return std::max(0.0, static_cast<double>(values[level]));
}

Vec3d RotateAroundAxis(const Vec3d& v, const Vec3d& axis, double angle) {
  if (angle == 0.0) {
    return v;
  }
  const Vec3d k = Normalize(axis);
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  return Add(Add(Mul(v, c), Mul(Cross(k, v), s)), Mul(k, Dot(k, v) * (1.0 - c)));
}

// 子枝挂到父枝上的位置：**用参数保存**，不是保存坐标。
// 父枝被拉长/弯走之后，按同一个参数重新求点，子枝才会跟着父枝走；
// 若改用「离父枝最近的点」，直的主干上点根本不会动（参数变了、位置没变）。
struct Attachment {
  bool valid = false;
  std::size_t segment = 0;  // 父枝折线的第几段
  double t = 0.0;           // 段内参数 [0,1]
};

Attachment FindAttachment(const Vec3d& point, const SampledPolyline& polyline) {
  Attachment best;
  if (polyline.points.size() < 2) {
    return best;
  }
  double best_distance = std::numeric_limits<double>::infinity();
  for (std::size_t k = 0; k + 1 < polyline.points.size(); ++k) {
    const Vec3d a = polyline.points[k];
    const Vec3d ab = Sub(polyline.points[k + 1], a);
    const double length_sqr = LenSqr(ab);
    double t = 0.0;
    if (length_sqr > 0.0) {
      t = std::clamp(Dot(Sub(point, a), ab) / length_sqr, 0.0, 1.0);
    }
    const double distance = Len(Sub(point, Add(a, Mul(ab, t))));
    if (distance < best_distance) {
      best_distance = distance;
      best.valid = true;
      best.segment = k;
      best.t = t;
    }
  }
  return best;
}

Vec3d EvaluatePolyline(const SampledPolyline& polyline, const Attachment& attachment) {
  if (polyline.points.empty()) {
    return Vec3d{ 0.0, 0.0, 0.0 };
  }
  const std::size_t segment =
      std::min(attachment.segment, polyline.points.size() - (polyline.points.size() > 1 ? 2 : 1));
  const Vec3d& a = polyline.points[segment];
  if (segment + 1 >= polyline.points.size()) {
    return a;
  }
  return Add(a, Mul(Sub(polyline.points[segment + 1], a), attachment.t));
}

std::vector<Vec3d> PolylineTangents(const std::vector<Vec3d>& points) {
  std::vector<Vec3d> tangents(points.size(), Vec3d{ 0.0, 1.0, 0.0 });
  for (std::size_t k = 0; k < points.size(); ++k) {
    if (points.size() < 2) {
      break;
    }
    const Vec3d delta = (k + 1 < points.size()) ? Sub(points[k + 1], points[k])
                                                : Sub(points[k], points[k - 1]);
    tangents[k] = Normalize(delta);
  }
  return tangents;
}

std::vector<double> NormalizedArclength(const std::vector<Vec3d>& points) {
  std::vector<double> cumulative(points.size(), 0.0);
  for (std::size_t k = 1; k < points.size(); ++k) {
    cumulative[k] = cumulative[k - 1] + Len(Sub(points[k], points[k - 1]));
  }
  const double total = cumulative.empty() ? 0.0 : cumulative.back();
  if (total > 0.0) {
    for (double& value : cumulative) {
      value /= total;
    }
  }
  return cumulative;
}

// ----------------------------- 枝条噪声（按「链」施加）-----------------------------
//
// Sapling 会把**同一根管子**切成好几段样条（后一段的起点就是前一段的终点，
// 附着参数 attach ≈ 1）。这些「延续样条」不是新枝，只是同一根枝的分段表示。
//
// 所以噪声必须按「链」施加，不能按单根样条：
//   * 尺寸因子逐样条不同 ⇒ 接头处粗细突变（实测 oak 218/565 个延续接头的半径比
//     偏离 1 超过 5%，肉眼就是「后一级没对准前一级」）；
//   * 弯曲逐样条各来一个大 S ⇒ 每个接头都是一道折角。
// 一条链共享：同一个缩放因子、同一个弯曲相位、同一段弧长参数化、同一个链根。

struct Chain {
  std::size_t level = 0;            // 噪声参数按链首的枝级取
  double length = 0.0;              // 干净几何上的链总长（米）
  Vec3d root{ 0.0, 0.0, 0.0 };      // 链首放置后的根点（缩放/弯曲之前）
  Vec3d offset{ 0.0, 0.0, 0.0 };    // 整条链的平移（链首贴到父枝的实际位移）
  Vec3d longitudinal{ 0.0, 1.0, 0.0 };
  Vec3d lateral{ 1.0, 0.0, 0.0 };
  double factor = 1.0;
  double radius_scale = 1.0;   // 接头半径修正（分叉处子链不许比父枝粗），整条链共享
  double phase = 0.0;
  double amplitude_longitudinal = 0.0;
  double amplitude_lateral = 0.0;
};

double PolylineLength(const std::vector<Vec3d>& points) {
  double total = 0.0;
  for (std::size_t k = 1; k < points.size(); ++k) {
    total += Len(Sub(points[k], points[k - 1]));
  }
  return total;
}

// 父枝在挂接点处的半径（用来保证分叉处子枝不比父枝粗）。
double EvaluateRadius(const SampledPolyline& polyline, const Attachment& attachment) {
  if (polyline.radii.empty()) {
    return 0.0;
  }
  const std::size_t segment = std::min(attachment.segment, polyline.radii.size() - 1);
  const double r0 = polyline.radii[segment];
  const double r1 = polyline.radii[std::min(segment + 1, polyline.radii.size() - 1)];
  return r0 + (r1 - r0) * attachment.t;
}

// 位移：d(t) = 系数 · 链长 · sin(2π(t + φ)) · sin²(πt)，t = 链内归一化弧长。
// sin² 窗口让整条链两端位移为 0（链根不离开父枝、链梢叶子不飘）。
// 注意 t 是**链**参数而不是单根样条的参数：延续接头两侧用同一个 t，位移相同，
// 接头处既不错位也不会出现折角。
void ApplyBendToChain(const Chain& chain, const std::vector<double>& chain_offsets,
                      SampledPolyline* polyline) {
  if (polyline->points.size() < 2 || chain.length <= 0.0) {
    return;
  }
  const double amplitude_length = chain.length * chain.factor;
  for (std::size_t k = 0; k < polyline->points.size(); ++k) {
    const double t = std::clamp(chain_offsets[k] / chain.length, 0.0, 1.0);
    const double envelope = std::sin(kPi * t);
    const double wave = std::sin(2.0 * kPi * (t + chain.phase));
    const double displacement = wave * envelope * envelope;
    polyline->points[k] =
        Add(polyline->points[k],
            Add(Mul(chain.longitudinal, chain.amplitude_longitudinal * amplitude_length *
                                            displacement),
                Mul(chain.lateral, chain.amplitude_lateral * amplitude_length * displacement)));
  }
}

// 按拓扑序施加噪声。链在**干净几何**上先切好（父枝归属、挂接参数都不受噪声影响）。
void ApplyBranchNoise(const NoiseOptions& noise, const Hierarchy& hierarchy,
                      const std::vector<int>& order, std::vector<SampledPolyline>* polys,
                      std::vector<std::vector<SampleRotation>>* rotations) {
  const std::size_t count = polys->size();
  rotations->assign(count, {});

  // 1) 干净几何上的挂接参数（父枝还没被动过）。
  std::vector<Attachment> attachments(count);
  for (std::size_t i = 0; i < count; ++i) {
    const int parent = hierarchy.parent[i];
    if (parent >= 0 && static_cast<std::size_t>(parent) < count && !(*polys)[i].points.empty()) {
      attachments[i] = FindAttachment((*polys)[i].points.front(), (*polys)[parent]);
    }
  }

  // 2) 分链 + 弧长参数化（都在干净几何上做，缩放是均匀的 ⇒ 归一化参数不变）。
  std::vector<int> chain_of(count, -1);
  std::vector<std::vector<double>> chain_offsets(count);
  std::vector<Chain> chains(count);
  for (const int i : order) {
    const int parent = hierarchy.parent[i];
    // 「延续」= 同一根管子的下一段，必须共享缩放因子/推移/弯曲参数。
    //
    // 判据不能只看 attach：attach 是按段数归一化的，采样一稀（低配 per_seg=2）
    // 真延续段会算成 0.93 之类；也不能只看"起点==父枝终点"—— Sapling 切段时
    // 两端会差几毫米。这里同时用三条，命中任一即是同一根管子：
    //   * attach 接近 1；
    //   * 起点落在父枝**末端** 2cm 内、且半径与父枝末端相差 < 2%。
    //     ⚠️ 必须是「父枝末端」而不是「父枝任意处」：同一根枝分叉出的两根子枝
    //     起点相同、半径也可能相同，按"离中心线近 + 半径同值"会把**兄弟枝**并成一条链，
    //     链长翻倍 ⇒ 弯度幅度跟着翻倍，整棵树会被推歪（实测 oak bbox 从 ±5m 涨到 12m）。
    bool same_tube = false;
    if (parent >= 0 && !(*polys)[i].points.empty() && !(*polys)[i].radii.empty() &&
        !(*polys)[parent].points.empty() && !(*polys)[parent].radii.empty()) {
      const double parent_radius = (*polys)[parent].radii.back();
      const double child_radius = (*polys)[i].radii.front();
      const double offset = Len(Sub((*polys)[i].points.front(), (*polys)[parent].points.back()));
      const bool radius_matches = parent_radius > 0.0 && child_radius > 0.0 &&
                                  std::fabs(child_radius / parent_radius - 1.0) < 0.02;
      same_tube = offset < 0.02 && radius_matches;
    }
    bool continuation =
        parent >= 0 && (hierarchy.attach[i] > kContinuationAttach || same_tube);
    // 父枝必须先被处理（拓扑序）。兜底：万一顺序异常，退回「当新链处理」，
    // 绝不让 chain_of[parent] = -1 拿去索引（那是越界写，会毁掉整棵树）。
    if (continuation && chain_of[parent] < 0) {
      continuation = false;
    }
    const int head = continuation ? chain_of[parent] : i;
    chain_of[i] = head;
    Chain& chain = chains[head];
    if (!continuation) {
      chain.level = static_cast<std::size_t>(std::max(0, hierarchy.level[i]));
      chain.length = 0.0;
    }
    const std::vector<Vec3d>& points = (*polys)[i].points;
    chain_offsets[i].assign(points.size(), 0.0);
    double local = 0.0;
    for (std::size_t k = 0; k < points.size(); ++k) {
      if (k > 0) {
        local += Len(Sub(points[k], points[k - 1]));
      }
      chain_offsets[i][k] = chain.length + local;
    }
    chain.length += local;
  }

  // 3) 每条链一份参数：因子/相位由 (seed, 链首编号) 派生；方向由链首切线给出。
  const Vec3d world_up{ 0.0, 1.0, 0.0 };
  for (const int i : order) {
    if (chain_of[i] != i) {
      continue;
    }
    Chain& chain = chains[i];
    const double jitter = LevelValue(noise.size_jitter, chain.level);
    chain.factor = 1.0;
    if (jitter > 0.0) {
      chain.factor =
          std::max(0.2, 1.0 + UnitFromHash(noise.seed, i, 0x9E3779B1u) * jitter);
    }
    chain.amplitude_longitudinal = LevelValue(noise.bend_longitudinal, chain.level);
    chain.amplitude_lateral = LevelValue(noise.bend_lateral, chain.level);
    chain.phase = 0.5 * (UnitFromHash(noise.seed, i, 0x85EBCA6Bu) + 1.0);

    const std::vector<Vec3d>& points = (*polys)[i].points;
    if (points.size() < 2) {
      continue;
    }
    const Vec3d tangent = Normalize(Sub(points[1], points[0]));
    Vec3d longitudinal = Sub(world_up, Mul(tangent, Dot(world_up, tangent)));
    if (Len(longitudinal) < 0.01) {
      // 枝几乎竖直时「世界竖直方向的投影」退化，改用 +Z 的同一投影。
      const Vec3d forward{ 0.0, 0.0, 1.0 };
      longitudinal = Sub(forward, Mul(tangent, Dot(forward, tangent)));
    }
    if (Len(longitudinal) < 1e-9) {
      longitudinal = Vec3d{ 1.0, 0.0, 0.0 };
    }
    chain.longitudinal = Normalize(longitudinal);
    chain.lateral = Normalize(Cross(tangent, chain.longitudinal));
  }

  // 4) 逐样条施加（父枝先于子枝，所以链首总能拿到已噪声的父枝位置）。
  for (const int i : order) {
    SampledPolyline& polyline = (*polys)[i];
    if (polyline.points.size() < 2) {
      continue;
    }
    const std::vector<Vec3d> tangents_before = PolylineTangents(polyline.points);
    Chain& chain = chains[chain_of[i]];
    const bool is_head = chain_of[i] == i;
    const bool has_parent = attachments[i].valid;

    // 4.1 平移：整条链共用同一个位移。
    //     * 链首是分叉：根点按**同一个挂接参数**贴到已噪声的父枝上，位移由它算出来；
    //     * 延续段：**必须同样平移**（它只是同一根管子的下一段，起点就是父枝的终点）。
    //       漏掉这一步就是「父枝挪了 1m、下一段还在原地」—— 实测 109/702 根枝脱开，
    //       最大 1.12m，肉眼正是「后一级没对准前一级」。
    if (is_head) {
      if (has_parent) {
        const int parent = hierarchy.parent[i];
        const Vec3d old_root = polyline.points.front();
        const Vec3d new_root = EvaluatePolyline((*polys)[parent], attachments[i]);
        chain.offset = Sub(new_root, old_root);
      } else {
        chain.offset = Vec3d{ 0.0, 0.0, 0.0 };
      }
      for (Vec3d& point : polyline.points) {
        point = Add(point, chain.offset);
      }
      chain.root = polyline.points.front();
    } else if (chain.offset.x != 0.0 || chain.offset.y != 0.0 || chain.offset.z != 0.0) {
      for (Vec3d& point : polyline.points) {
        point = Add(point, chain.offset);
      }
    }


    // 4.2 尺寸：整条链按**链根**缩放（长度与半径乘同一个因子）。
    if (chain.factor != 1.0) {
      for (Vec3d& point : polyline.points) {
        point = Add(chain.root, Mul(Sub(point, chain.root), chain.factor));
      }
      for (double& radius : polyline.radii) {
        radius *= chain.factor;
      }
    }

    // 4.3 分叉接头：子链根不许比父枝还粗（原本 Sapling 保证 ≤ 父枝，抖动会破坏它）。
    //     修正量挂在**链**上、整条链一起乘 —— 只压链首那一段的话，接头另一侧
    //     （延续段）没被压，半径比就从 1.00 变成 1.22，肉眼又是一道台阶。
    if (is_head && has_parent) {
      const int parent = hierarchy.parent[i];
      const double parent_radius = EvaluateRadius((*polys)[parent], attachments[i]);
      const double root_radius = polyline.radii.empty() ? 0.0 : polyline.radii.front();
      const double limit = 0.95 * parent_radius;
      if (parent_radius > 0.0 && root_radius > limit && root_radius > 0.0) {
        chain.radius_scale = limit / root_radius;
      }
    }
    if (chain.radius_scale != 1.0) {
      for (double& radius : polyline.radii) {
        radius *= chain.radius_scale;
      }
    }

    // 4.4 弯度：按链的弧长参数化（延续接头两侧 t 相同 ⇒ 位移相同，不会折角）。
    if (chain.amplitude_longitudinal != 0.0 || chain.amplitude_lateral != 0.0) {
      ApplyBendToChain(chain, chain_offsets[i], &polyline);
    }

    // 4.5 姿态旋转表：把「噪声前的切线」转到「噪声后的切线」（最短弧），给叶卡用。
    const std::vector<Vec3d> tangents_after = PolylineTangents(polyline.points);
    std::vector<SampleRotation>& branch_rotations = (*rotations)[i];
    branch_rotations.resize(polyline.points.size());
    for (std::size_t k = 0; k < branch_rotations.size(); ++k) {
      const Vec3d& before = tangents_before[k];
      const Vec3d& after = tangents_after[k];
      const double cosine = std::clamp(Dot(before, after), -1.0, 1.0);
      const double angle = std::acos(cosine);
      if (angle < 1e-9) {
        branch_rotations[k] = SampleRotation{};
        continue;
      }
      Vec3d axis = Cross(before, after);
      if (Len(axis) < 1e-9) {
        axis = Cross(before, Vec3d{ 0.0, 1.0, 0.0 });
        if (Len(axis) < 1e-9) {
          axis = Cross(before, Vec3d{ 1.0, 0.0, 0.0 });
        }
      }
      branch_rotations[k] = SampleRotation{ Normalize(axis), angle };
    }
  }
}

// 镜像手性的素材必须左右翻转，锚角也跟着左右换。
std::string MirroredAnchorMode(const std::string& mode) {
  if (mode == "bottom_left") {
    return "bottom_right";
  }
  if (mode == "bottom_right") {
    return "bottom_left";
  }
  return mode;  // bottom_center / top_center 左右对称
}

}  // namespace

bool BuildShape(const SaplingDump& dump, const BuildOptions& options, Shape* out /*output*/,
                BuildStats* stats /*output*/, std::string* error /*output*/) {
  if (dump.splines.empty()) {
    *error = "dump 里没有样条";
    return false;
  }
  if (options.per_seg == 0) {
    *error = "per_seg 必须 > 0";
    return false;
  }
  if (dump.leaf_vertices.size() % 4 != 0) {
    *error = "叶卡网格顶点数 " + std::to_string(dump.leaf_vertices.size()) +
             " 不是 4 的倍数，无法按卡切分";
    return false;
  }

  // ---- 采样（Blender 坐标系）----
  std::vector<SampledPolyline> polys;
  polys.reserve(dump.splines.size());
  for (const DumpSpline& spline : dump.splines) {
    polys.push_back(SampleSpline(spline, dump.bevel_depth, options.per_seg));
  }
  // ---- 资产格式坐标系（+Y up），叶卡与输出都用它 ----
  std::vector<SampledPolyline> polys_up;
  polys_up.reserve(polys.size());
  for (const SampledPolyline& polyline : polys) {
    SampledPolyline up;
    up.radii = polyline.radii;
    up.points.reserve(polyline.points.size());
    for (const Vec3d& point : polyline.points) {
      up.points.push_back(ToUp(point));
    }
    polys_up.push_back(std::move(up));
  }

  // ---- 层级 + 拓扑序 ----
  const Hierarchy hierarchy = BuildHierarchy(polys);
  const std::vector<int> order = TopologicalOrder(hierarchy.parent);

  // ---- 枝条噪声（只改 polys_up：层级已经在干净几何上算好，避免噪声改变父枝归属）----
  std::vector<std::vector<SampleRotation>> sample_rotations;
  if (options.noise.Enabled()) {
    ApplyBranchNoise(options.noise, hierarchy, order, &polys_up, &sample_rotations);
  }
  std::vector<int> new_id(hierarchy.parent.size(), -1);
  for (std::size_t position = 0; position < order.size(); ++position) {
    new_id[static_cast<std::size_t>(order[position])] = static_cast<int>(position);
  }

  // ---- 叶卡 ----
  std::vector<BuiltCard> cards;
  if (!dump.leaf_vertices.empty()) {
    // 骨架采样点的「点云」查询：叶卡的 anchor 取**最近的采样点**（不是最近线段），
    // 与 Python 版一致（这样同一圈上的顶点拿到同一个 anchor，卡片不会被剪切）。
    std::vector<std::vector<geom::Vec3<double>>> point_cloud;
    std::vector<int> point_branch;
    std::vector<int> point_local;
    for (std::size_t i = 0; i < polys_up.size(); ++i) {
      for (std::size_t k = 0; k < polys_up[i].points.size(); ++k) {
        point_cloud.push_back({ ToGeom(polys_up[i].points[k]) });
        point_branch.push_back(static_cast<int>(i));
        point_local.push_back(static_cast<int>(k));
      }
    }
    const double cell_size = std::max(Diagonal(polys) / 64.0, 1e-6);
    const jpov::util::NearestPointMatcher3d<double> point_matcher(std::move(point_cloud), cell_size);

    const std::size_t card_count = dump.leaf_vertices.size() / 4;
    cards.reserve(card_count);
    double worst_anchor_error = 0.0;
    int bad_anchors = 0;
    std::string first_bad;

    for (std::size_t card_index = 0; card_index < card_count; ++card_index) {
      CardCorners quad;
      for (int k = 0; k < 4; ++k) {
        const DumpLeafVertex& vertex = dump.leaf_vertices[card_index * 4 + static_cast<std::size_t>(k)];
        quad.corner[k] = ToUp(vertex.position);
        quad.u[k] = vertex.u;
        quad.v[k] = vertex.v;
        if (vertex.u < 0.0 || vertex.v < 0.0) {
          quad.has_uv = false;
        }
      }
      const Vec3d centre = Mul(Add(Add(quad.corner[0], quad.corner[1]),
                                   Add(quad.corner[2], quad.corner[3])),
                               0.25);
      const std::optional<jpov::util::NearestPointMatcher3d<double>::Match> nearest =
          point_matcher.Query(ToGeom(centre));
      if (!nearest.has_value()) {
        continue;
      }
      const int point_index = nearest->polyline;
      const int branch = point_branch[static_cast<std::size_t>(point_index)];
      const int local = point_local[static_cast<std::size_t>(point_index)];
      const Vec3d anchor = polys_up[static_cast<std::size_t>(branch)]
                               .points[static_cast<std::size_t>(local)];

      const Vec3d mid_a = Mul(Add(quad.corner[0], quad.corner[1]), 0.5);
      const Vec3d mid_b = Mul(Add(quad.corner[2], quad.corner[3]), 0.5);
      // 根边 = 离枝更近的那条对边（叶柄在根部）。
      // 根边 = 离枝更近的那条对边（叶柄在根部）。
      // 卡片正好骑在枝上时两条边**等距**（anchor 落在卡片对称面上），方向在数学上
      // 没有定义；float32（Blender）与 double（这里）的末位差会让 ~1% 的卡取反，
      // 连带手性 type 一起翻。这是这个判据的固有歧义，不是实现错误 ——
      // 试过加容差强行统一，反而把不一致从 45 张涨到 214 张，所以保持朴素比较。
      const bool which = Len(Sub(mid_a, anchor)) < Len(Sub(mid_b, anchor)) ? 0 : 1;
      const Vec3d root_mid = which == 0 ? mid_a : mid_b;
      const Vec3d tip_mid = which == 0 ? mid_b : mid_a;
      Vec3d up = Normalize(Sub(tip_mid, root_mid));
      const double height = Len(Sub(tip_mid, root_mid));
      if (height <= kMinCardHeight) {
        continue;
      }
      const Vec3d width_vec =
          which == 0 ? Sub(quad.corner[1], quad.corner[0]) : Sub(quad.corner[3], quad.corner[2]);
      Vec3d normal = Normalize(Cross(width_vec, up));

      BuiltCard card;
      card.anchor = anchor;
      card.up = up;
      card.normal = normal;
      card.height = height;
      card.branch = branch;
      const std::size_t branch_points = polys_up[static_cast<std::size_t>(branch)].points.size();
      card.s = static_cast<double>(local) /
               static_cast<double>(std::max<std::size_t>(1, branch_points - 1));

      // 手性：贴图 +u 方向与 right = cross(up, normal) 同向则是未镜像。
      if (quad.has_uv) {
        int u_min = 0;
        int u_max = 0;
        for (int k = 1; k < 4; ++k) {
          if (quad.u[k] < quad.u[u_min]) {
            u_min = k;
          }
          if (quad.u[k] > quad.u[u_max]) {
            u_max = k;
          }
        }
        const Vec3d u_dir = Sub(quad.corner[u_max], quad.corner[u_min]);
        const Vec3d right = Cross(up, normal);
        card.mirrored = Dot(Normalize(u_dir), right) < 0.0;
      }

      // 叶卡姿态跟着枝走：枝被弯了，卡片也要绕连接点转到新的切线方向上，
      // 否则叶子仍朝着弯曲前的方向，粗枝上会明显穿进树干。
      // 注意必须在上面的手性判定**之后**做旋转：手性比的是素材自身的 +u
      // 与世界 right 的关系，用未旋转的姿态才和 Blender 里看到的一致。
      if (static_cast<std::size_t>(branch) < sample_rotations.size()) {
        const std::vector<SampleRotation>& branch_rotations =
            sample_rotations[static_cast<std::size_t>(branch)];
        if (static_cast<std::size_t>(local) < branch_rotations.size()) {
          const SampleRotation& rotation = branch_rotations[static_cast<std::size_t>(local)];
          if (rotation.angle != 0.0) {
            up = RotateAroundAxis(up, rotation.axis, rotation.angle);
            normal = RotateAroundAxis(normal, rotation.axis, rotation.angle);
          }
        }
      }

      // anchor 合同：必须落在所属枝条上（距离 ≤ 该处半径 + 容差）。
      double branch_radius = 0.0;
      const double distance =
          DistanceToPolyline(anchor, polys_up[static_cast<std::size_t>(branch)], &branch_radius);
      const double allowed = branch_radius + kAnchorSlack;
      if (distance > allowed) {
        ++bad_anchors;
        worst_anchor_error = std::max(worst_anchor_error, distance - allowed);
        if (first_bad.empty()) {
          first_bad = "卡 " + std::to_string(card_index) + " 离枝 " +
                      std::to_string(distance) + " m（枝半径 " + std::to_string(branch_radius) +
                      " m）";
        }
        continue;
      }
      cards.push_back(card);
    }
    if (bad_anchors > 0) {
      *error = std::to_string(bad_anchors) + " 张叶卡的 anchor 不在所属枝条上（" + first_bad +
               "，最大超出 " + std::to_string(worst_anchor_error) +
               " m）—— anchor 必须是叶子与枝条的连接点，不能靠它做 UV 补偿";
      return false;
    }
  }

  // ---- 组装 ----
  *out = Shape();
  out->version = 1;
  out->description = options.species + "（Blender/Sapling 导出，per_seg=" +
                     std::to_string(options.per_seg) + "，" +
                     std::to_string(polys.size()) + " 枝 " + std::to_string(cards.size()) + " 卡）";

  out->branches.reserve(order.size());
  for (std::size_t position = 0; position < order.size(); ++position) {
    const int source = order[position];
    const SampledPolyline& polyline = polys_up[static_cast<std::size_t>(source)];
    Branch branch;
    branch.id = static_cast<uint32_t>(position);
    branch.parent = hierarchy.parent[static_cast<std::size_t>(source)] < 0
                        ? -1
                        : new_id[static_cast<std::size_t>(
                              hierarchy.parent[static_cast<std::size_t>(source)])];
    branch.level = static_cast<uint32_t>(hierarchy.level[static_cast<std::size_t>(source)]);
    {
      const std::vector<uint32_t>& rings = options.ring_counts;
      const std::size_t level = static_cast<std::size_t>(branch.level);
      const uint32_t ring = rings.empty()
                                ? 8u
                                : rings[std::min(level, rings.size() - 1)];
      branch.ring_count = std::max(3u, ring);
    }
    branch.seam_index = 0;
    branch.samples.reserve(polyline.points.size());
    for (std::size_t k = 0; k < polyline.points.size(); ++k) {
      BranchSample sample;
      sample.position = polyline.points[k];
      sample.radius = polyline.radii[k];
      sample.tilt = 0.0;
      branch.samples.push_back(std::move(sample));
    }
    out->branches.push_back(std::move(branch));
  }

  int plain_count = 0;
  int mirrored_count = 0;
  for (const BuiltCard& card : cards) {
    if (card.mirrored) {
      ++mirrored_count;
    } else {
      ++plain_count;
    }
  }
  const bool has_plain = plain_count > 0;
  const uint32_t mirrored_type = has_plain ? 1u : 0u;
  if (has_plain) {
    LeafType type;
    type.type = 0;
    type.description = options.leaf_type_desc;
    type.anchor_mode = options.anchor_mode;
    type.bend = options.leaf_bend;
    out->leaf_types.push_back(type);
  }
  if (mirrored_count > 0) {
    LeafType type;
    type.type = mirrored_type;
    const std::string mode =
        has_plain ? MirroredAnchorMode(options.anchor_mode) : options.anchor_mode;
    type.description = options.leaf_type_desc + "（镜像手性：素材左右翻转后使用，根部在格子 " +
                       mode + " 角）";
    type.anchor_mode = mode;
    // 弯曲与手性无关：s 是到中脉线的距离（左右对称），镜像不用翻符号。
    type.bend = options.leaf_bend;
    out->leaf_types.push_back(type);
  }

  out->cards.reserve(cards.size());
  for (const BuiltCard& built : cards) {
    LeafCard card;
    card.anchor = built.anchor;
    card.up = built.up;
    card.normal = built.normal;
    card.height = built.height;
    card.width = built.height * options.leaf_aspect;
    card.type = built.mirrored ? mirrored_type : 0u;
    card.branch = static_cast<uint32_t>(new_id[static_cast<std::size_t>(built.branch)]);
    card.s = built.s;
    out->cards.push_back(std::move(card));
  }

  stats->branches = static_cast<int>(out->branches.size());
  stats->cards = static_cast<int>(out->cards.size());
  stats->mirrored_cards = mirrored_count;
  return true;
}

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov

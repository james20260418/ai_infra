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
      const Vec3d up = Normalize(Sub(tip_mid, root_mid));
      const double height = Len(Sub(tip_mid, root_mid));
      if (height <= kMinCardHeight) {
        continue;
      }
      const Vec3d width_vec =
          which == 0 ? Sub(quad.corner[1], quad.corner[0]) : Sub(quad.corner[3], quad.corner[2]);
      const Vec3d normal = Normalize(Cross(width_vec, up));

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
    branch.ring_count = branch.level == 0 ? 16u : (branch.level == 1 ? 12u : 8u);
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

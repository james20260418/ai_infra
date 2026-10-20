// 植物拓扑结构的实现：proto 转换 + 合法性校验。

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

namespace jpov {
namespace vegetation {

namespace {

constexpr double kPi = 3.14159265358979323846;  // 不依赖 M_PI（MinGW 下未定义）
constexpr double kUnitEps = 1e-3;   // 单位向量/正交性的容差
constexpr double kDotEps = 2e-3;    // up·normal 的容差

double Length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool NearlyEqual(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return std::string(buf);
}

// 截面必须能当"星形多边形"用：从中心射出的任意方向只交一次。
// 判据是顶点极角严格单调递增（逆时针），这比逐段求交简单，且能覆盖凸多边形。
bool IsStarShapedCounterClockwise(const std::vector<ProfilePoint>& points,
                                  std::string* why) {
  double area2 = 0.0;
  for (size_t i = 0; i < points.size(); ++i) {
    const ProfilePoint& a = points[i];
    const ProfilePoint& b = points[(i + 1) % points.size()];
    area2 += a.x * b.y - b.x * a.y;
  }
  if (area2 <= 0.0) {
    *why = "profile 顶点不是逆时针（有向面积为负）";
    return false;
  }
  // 极角单调性：把每个顶点角度展开成非递减序列，允许跨 ±π 一次。
  // 循环到 i == n 是刻意的：最后一圈要回到 points[0]，否则少算一条边的角度
  // （N 个点只累加 N-1 条边，总和恒为 2π(1-1/N)，会被误判成"没绕满一圈"）。
  const size_t n = points.size();
  double prev = std::atan2(points[0].y, points[0].x);
  double total = 0.0;
  for (size_t i = 1; i <= n; ++i) {
    const ProfilePoint& p = points[i % n];
    double cur = std::atan2(p.y, p.x);
    double delta = cur - prev;
    while (delta <= -kPi) {
      delta += 2.0 * kPi;
    }
    while (delta > kPi) {
      delta -= 2.0 * kPi;
    }
    if (delta <= 0.0) {
      *why = Format("profile 极角在顶点 %zu 处不递增（不是星形/或顶点顺序反了）", i % n);
      return false;
    }
    total += delta;
    prev = cur;
  }
  if (!NearlyEqual(total, 2.0 * kPi, 1e-2)) {
    *why = Format("profile 极角总增量 %.4f != 2pi（轮廓可能没绕满一圈）", total);
    return false;
  }
  return true;
}

}  // namespace

double CrossSection::Perimeter() const {
  if (points.size() < 2) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < points.size(); ++i) {
    const ProfilePoint& a = points[i];
    const ProfilePoint& b = points[(i + 1) % points.size()];
    sum += std::hypot(b.x - a.x, b.y - a.y);
  }
  return sum;
}

uint32_t Branch::EffectiveRingCount(const CrossSection& profile) const {
  if (ring_count > 0) {
    return ring_count;
  }
  return static_cast<uint32_t>(profile.points.size());
}

Shape FromProto(const ShapeProto& proto) {
  Shape shape;
  shape.format = proto.format();
  shape.version = proto.version();
  shape.units = proto.units();
  shape.up_axis = proto.up_axis();

  shape.profile.seam_index = proto.profile().seam_index();
  shape.profile.points.reserve(proto.profile().points_size());
  for (const ProfilePointProto& p : proto.profile().points()) {
    shape.profile.points.push_back(ProfilePoint{p.x(), p.y()});
  }

  shape.branches.reserve(proto.branches_size());
  for (const BranchProto& b : proto.branches()) {
    Branch branch;
    branch.id = b.id();
    branch.parent = b.parent();
    branch.level = b.level();
    branch.ring_count = b.ring_count();
    branch.samples.reserve(b.samples_size());
    for (const BranchSampleProto& s : b.samples()) {
      branch.samples.push_back(
          BranchSample{Vec3d{s.position().x(), s.position().y(), s.position().z()},
                       s.radius(), s.tilt()});
    }
    shape.branches.push_back(std::move(branch));
  }

  shape.leaf_types.reserve(proto.leaf_types_size());
  for (const LeafTypeProto& t : proto.leaf_types()) {
    LeafType type;
    type.type = t.type();
    type.description = t.description();
    type.anchor_mode = t.anchor_mode();
    type.preferred_aspect = t.preferred_aspect();
    shape.leaf_types.push_back(std::move(type));
  }

  shape.cards.reserve(proto.cards_size());
  for (const LeafCardProto& c : proto.cards()) {
    LeafCard card;
    card.anchor = Vec3d{c.anchor().x(), c.anchor().y(), c.anchor().z()};
    card.up = Vec3d{c.up().x(), c.up().y(), c.up().z()};
    card.normal = Vec3d{c.normal().x(), c.normal().y(), c.normal().z()};
    card.height = c.height();
    card.type = c.type();
    card.branch = c.branch();
    card.s = c.s();
    shape.cards.push_back(std::move(card));
  }

  return shape;
}

bool Validate(const Shape& shape, std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };

  // ── 全局约定 ──
  if (shape.format != "jpov.vegetation") {
    return fail(Format("format 必须是 jpov.vegetation，实际 \"%s\"",
                       shape.format.c_str()));
  }
  if (shape.units != "meters") {
    return fail(Format("units 必须是 meters，实际 \"%s\"", shape.units.c_str()));
  }
  if (shape.up_axis != "+Z") {
    return fail(Format("up_axis 必须是 +Z，实际 \"%s\"", shape.up_axis.c_str()));
  }
  if (shape.version == 0) {
    return fail("version 不能为 0");
  }

  // ── 截面 ──
  if (shape.profile.points.size() < 3) {
    return fail(Format("profile 至少 3 个点，实际 %zu", shape.profile.points.size()));
  }
  if (shape.profile.seam_index >= shape.profile.points.size()) {
    return fail(Format("profile.seam_index %u 越界（点数 %zu）",
                       shape.profile.seam_index, shape.profile.points.size()));
  }
  std::string why;
  if (!IsStarShapedCounterClockwise(shape.profile.points, &why)) {
    return fail(why);
  }
  if (!(shape.profile.Perimeter() > 0.0)) {
    return fail("profile 周长为 0");
  }

  // ── 枝干拓扑 ──
  if (shape.branches.empty()) {
    return fail("branches 为空");
  }
  std::set<uint32_t> ids;
  int root_count = 0;
  for (const Branch& b : shape.branches) {
    if (!ids.insert(b.id).second) {
      return fail(Format("branch id %u 重复", b.id));
    }
    if (b.parent == -1) {
      ++root_count;
    } else if (b.parent < 0) {
      return fail(Format("branch %u: parent 只能是 -1 或合法 id，实际 %d",
                         b.id, b.parent));
    } else if (static_cast<uint32_t>(b.parent) >= b.id) {
      // 要求拓扑序：父在前。这样"沿父链累加弧长"一趟就能算完，也不需要防环。
      return fail(Format("branch %u: parent %d 未出现在自身之前（需拓扑序）",
                         b.id, b.parent));
    }
  }
  if (root_count != 1) {
    return fail(Format("恰好需要 1 根根枝（parent=-1），实际 %d", root_count));
  }
  for (const Branch& b : shape.branches) {
    if (b.samples.size() < 2) {
      return fail(Format("branch %u: samples 至少 2 个，实际 %zu",
                         b.id, b.samples.size()));
    }
    for (size_t i = 0; i < b.samples.size(); ++i) {
      if (!(b.samples[i].radius > 0.0)) {
        return fail(Format("branch %u sample %zu: radius 必须 > 0，实际 %g",
                           b.id, i, b.samples[i].radius));
      }
    }
    if (b.parent >= 0 && ids.count(static_cast<uint32_t>(b.parent)) == 0) {
      return fail(Format("branch %u: parent %d 不存在", b.id, b.parent));
    }
  }

  // ── 叶卡类型 ──
  std::set<uint32_t> type_ids;
  for (const LeafType& t : shape.leaf_types) {
    if (!type_ids.insert(t.type).second) {
      return fail(Format("leaf_type %u 重复", t.type));
    }
    if (t.anchor_mode != "bottom_center" && t.anchor_mode != "top_center" &&
        t.anchor_mode != "center") {
      return fail(Format("leaf_type %u: anchor_mode \"%s\" 非法", t.type,
                         t.anchor_mode.c_str()));
    }
    if (t.preferred_aspect < 0.0) {
      return fail(Format("leaf_type %u: preferred_aspect 不能为负", t.type));
    }
  }

  // ── 叶卡 ──
  for (size_t i = 0; i < shape.cards.size(); ++i) {
    const LeafCard& c = shape.cards[i];
    const double up_len = Length(c.up);
    if (!NearlyEqual(up_len, 1.0, kUnitEps)) {
      return fail(Format("card %zu: up 必须是单位向量，|up| = %.6f", i, up_len));
    }
    const double n_len = Length(c.normal);
    if (!NearlyEqual(n_len, 1.0, kUnitEps)) {
      return fail(Format("card %zu: normal 必须是单位向量，|normal| = %.6f", i, n_len));
    }
    const double dot = c.up.x * c.normal.x + c.up.y * c.normal.y + c.up.z * c.normal.z;
    if (std::fabs(dot) > kDotEps) {
      return fail(Format("card %zu: up·normal = %.6f，需正交", i, dot));
    }
    if (!(c.height > 0.0)) {
      return fail(Format("card %zu: height 必须 > 0，实际 %g", i, c.height));
    }
    if (type_ids.count(c.type) == 0) {
      return fail(Format("card %zu: type %u 不在 leaf_types 里", i, c.type));
    }
    if (ids.count(c.branch) == 0) {
      return fail(Format("card %zu: branch %u 不存在", i, c.branch));
    }
    if (!(c.s >= 0.0 && c.s <= 1.0)) {
      return fail(Format("card %zu: s 必须落在 [0,1]，实际 %g", i, c.s));
    }
  }

  return true;
}

}  // namespace vegetation
}  // namespace jpov

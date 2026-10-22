// 植物拓扑结构的实现：proto 转换 + 派生计算 + 合法性校验。

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <google/protobuf/text_format.h>

namespace jpov {
namespace vegetation {

namespace {

constexpr double kPi = 3.14159265358979323846;  // 不依赖 M_PI（MinGW 下未定义）
constexpr double kUnitEps = 1e-3;   // 单位向量/正交性的容差
constexpr double kDotEps = 2e-3;    // up·normal 的容差
// anchor 必须落在所属枝条上的容差（米）：叶柄可以插进管壁一点、导出用的采样折线
// 也可能比真实曲面低几毫米，所以给「枝半径 + 1cm」。**不是**给「随便偏一点也行」——
// 曾经的 bug 是 anchor 偏出 13cm（整卡被抬了 h/2），这条校验就是拦它的。
constexpr double kAnchorSlackMeters = 0.01;

double Length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool NearlyEqual(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// 点到折线的最近距离，同时给出最近点处的插值半径。
// 返回距离；*radius 写入最近点所在段的插值半径（samples 至少 2 个）。
double DistanceToPolyline(const Vec3d& p, const std::vector<BranchSample>& samples,
                          double* radius) {
  double best = 1e300;
  for (size_t i = 0; i + 1 < samples.size(); ++i) {
    const Vec3d& a = samples[i].position;
    const Vec3d& b = samples[i + 1].position;
    const double abx = b.x - a.x;
    const double aby = b.y - a.y;
    const double abz = b.z - a.z;
    const double len2 = abx * abx + aby * aby + abz * abz;
    double t = 0.0;
    if (len2 > 0.0) {
      t = ((p.x - a.x) * abx + (p.y - a.y) * aby + (p.z - a.z) * abz) / len2;
      t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
    }
    const double dx = p.x - (a.x + abx * t);
    const double dy = p.y - (a.y + aby * t);
    const double dz = p.z - (a.z + abz * t);
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < best) {
      best = d;
      if (radius != nullptr) {
        *radius = samples[i].radius + (samples[i + 1].radius - samples[i].radius) * t;
      }
    }
  }
  return best;
}

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

// 单位轮廓的周长（未乘 radius）。
double UnitPerimeter(const std::vector<ProfilePoint>& points) {
  double sum = 0.0;
  for (size_t i = 0; i < points.size(); ++i) {
    const ProfilePoint& a = points[i];
    const ProfilePoint& b = points[(i + 1) % points.size()];
    sum += std::hypot(b.x - a.x, b.y - a.y);
  }
  return sum;
}

}  // namespace

bool AnchorModeToUv(const std::string& mode, double* u, double* v) {
  if (mode == "bottom_left") {
    *u = 0.0;
    *v = 0.0;
  } else if (mode == "bottom_center") {
    *u = 0.5;
    *v = 0.0;
  } else if (mode == "bottom_right") {
    *u = 1.0;
    *v = 0.0;
  } else if (mode == "top_center") {
    *u = 0.5;
    *v = 1.0;
  } else {
    return false;
  }
  return true;
}

std::vector<ProfilePoint> UnitCircleProfile(uint32_t n) {
  std::vector<ProfilePoint> points;
  points.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
    points.push_back(ProfilePoint{std::cos(a), std::sin(a)});
  }
  return points;
}

uint32_t Branch::EffectiveRingCount() const {
  if (ring_count > 0) {
    return ring_count;
  }
  for (const BranchSample& s : samples) {
    if (!s.profile.empty()) {
      return static_cast<uint32_t>(s.profile.size());
    }
  }
  return 0;
}

Shape FromProto(const ShapeProto& proto) {
  Shape shape;
  shape.version = proto.version();
  shape.description = proto.description();

  shape.branches.reserve(proto.branches_size());
  for (const BranchProto& b : proto.branches()) {
    Branch branch;
    branch.id = b.id();
    branch.parent = b.parent();
    branch.level = b.level();
    branch.ring_count = b.ring_count();
    branch.seam_index = b.seam_index();
    branch.samples.reserve(b.samples_size());
    for (const BranchSampleProto& s : b.samples()) {
      BranchSample sample;
      sample.position = Vec3d{s.position().x(), s.position().y(), s.position().z()};
      sample.radius = s.radius();
      sample.tilt = s.tilt();
      sample.profile.reserve(s.profile_size());
      for (const ProfilePointProto& p : s.profile()) {
        sample.profile.push_back(ProfilePoint{p.x(), p.y()});
      }
      branch.samples.push_back(std::move(sample));
    }
    shape.branches.push_back(std::move(branch));
  }

  shape.leaf_types.reserve(proto.leaf_types_size());
  for (const LeafTypeProto& t : proto.leaf_types()) {
    LeafType type;
    type.type = t.type();
    type.description = t.description();
    type.anchor_mode = t.anchor_mode();
    shape.leaf_types.push_back(std::move(type));
  }

  shape.cards.reserve(proto.cards_size());
  for (const LeafCardProto& c : proto.cards()) {
    LeafCard card;
    card.anchor = Vec3d{c.anchor().x(), c.anchor().y(), c.anchor().z()};
    card.up = Vec3d{c.up().x(), c.up().y(), c.up().z()};
    card.normal = Vec3d{c.normal().x(), c.normal().y(), c.normal().z()};
    card.height = c.height();
    card.width = c.width();
    card.type = c.type();
    card.branch = c.branch();
    card.s = c.s();
    shape.cards.push_back(std::move(card));
  }

  return shape;
}

bool LoadShapeFile(const std::string& path, Shape* out, std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };
  if (out == nullptr) {
    return fail("LoadShapeFile: out 不能为空");
  }
  std::ifstream in(path);
  if (!in) {
    return fail("打不开 shape 文件: " + path);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  ShapeProto proto;
  if (!google::protobuf::TextFormat::ParseFromString(ss.str(), &proto)) {
    return fail("解析 ShapeProto 文本失败（字段名/括号与 proto/shape.proto 对不上？）: " + path);
  }
  *out = FromProto(proto);
  std::string why;
  if (!Validate(*out, &why)) {
    return fail("shape 校验失败: " + why + "（文件: " + path + "）");
  }
  return true;
}

bool Validate(const Shape& shape, std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };

  // ── 版本 ──
  // 单位（米）与上方向（+Y）是硬编码约定，没有字段可校验；越界值只可能来自
  // 生成器写错，由下游几何结果暴露（树大 1000 倍 / 树躺倒），故此处只查版本。
  if (shape.version == 0) {
    return fail("version 不能为 0");
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

  // ── 逐枝逐点 ──
  for (const Branch& b : shape.branches) {
    if (b.parent >= 0 && ids.count(static_cast<uint32_t>(b.parent)) == 0) {
      return fail(Format("branch %u: parent %d 不存在", b.id, b.parent));
    }
    if (b.samples.size() < 2) {
      return fail(Format("branch %u: samples 至少 2 个，实际 %zu",
                         b.id, b.samples.size()));
    }
    const uint32_t ring = b.EffectiveRingCount();
    if (ring < 3) {
      return fail(Format("branch %u: 无法确定环顶点数（ring_count=0 且没有 profile）",
                         b.id));
    }
    if (b.seam_index >= ring) {
      return fail(Format("branch %u: seam_index %u 越界（环顶点数 %u）",
                         b.id, b.seam_index, ring));
    }
    for (size_t i = 0; i < b.samples.size(); ++i) {
      const BranchSample& s = b.samples[i];
      if (!(s.radius > 0.0)) {
        return fail(Format("branch %u sample %zu: radius 必须 > 0，实际 %g",
                           b.id, i, s.radius));
      }
      if (s.profile.empty()) {
        continue;  // 空 = 正圆，由 ring_count 兜底
      }
      if (s.profile.size() < 3) {
        return fail(Format("branch %u sample %zu: profile 至少 3 个点，实际 %zu",
                           b.id, i, s.profile.size()));
      }
      if (s.profile.size() != ring) {
        // 环点数必须逐点一致，否则扫管时相邻两圈接不上（三角化会错位）。
        return fail(Format("branch %u sample %zu: profile 点数 %zu != 环顶点数 %u",
                           b.id, i, s.profile.size(), ring));
      }
      std::string why;
      if (!IsStarShapedCounterClockwise(s.profile, &why)) {
        return fail(Format("branch %u sample %zu: %s", b.id, i, why.c_str()));
      }
      if (!(UnitPerimeter(s.profile) > 0.0)) {
        return fail(Format("branch %u sample %zu: profile 周长为 0", b.id, i));
      }
    }
  }

  // ── 叶卡类型 ──
  std::set<uint32_t> type_ids;
  for (const LeafType& t : shape.leaf_types) {
    if (!type_ids.insert(t.type).second) {
      return fail(Format("leaf_type %u 重复", t.type));
    }
    if (!IsValidAnchorMode(t.anchor_mode)) {
      return fail(Format("leaf_type %u: anchor_mode \"%s\" 非法"
                         "（只接受 bottom_left/bottom_center/bottom_right/top_center）",
                         t.type, t.anchor_mode.c_str()));
    }
  }

  // ── 叶卡 ──
  // 需要按 id 取枝条几何来校验 anchor 落在枝上，这里建一次索引（枝数远小于卡数）。
  std::unordered_map<uint32_t, const Branch*> branch_by_id;
  for (const Branch& b : shape.branches) {
    branch_by_id.emplace(b.id, &b);
  }
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
    if (!(c.width > 0.0)) {
      return fail(Format("card %zu: width 必须 > 0（装配时由 height × 格子宽/高 得来）", i));
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
    // anchor 的语义是「叶子与枝条的连接点」，**必须真的落在所属枝条上**。
    // 否则 anchor + anchor_mode 推出来的 UV 锚点会落在卡片中间，素材再怎么画都不可能贴枝
    // （历史 bug：导出把 anchor 写成「根边中点 = 卡片中心 − up·h/2」，442 张卡全偏 12~18cm）。
    {
      double radius = 0.0;
      const double dist = DistanceToPolyline(c.anchor, branch_by_id.at(c.branch)->samples,
                                             &radius);
      if (dist > radius + kAnchorSlackMeters) {
        return fail(Format("card %zu: anchor 不在所属枝条 %u 上（到中心线 %.4f m，"
                           "枝半径 %.4f m，容差 %.4f m）—— anchor 必须是叶子与枝条的连接点",
                           i, c.branch, dist, radius, radius + kAnchorSlackMeters));
      }
    }
    if (!(c.s >= 0.0 && c.s <= 1.0)) {
      return fail(Format("card %zu: s 必须落在 [0,1]，实际 %g", i, c.s));
    }
  }

  return true;
}

}  // namespace vegetation
}  // namespace jpov

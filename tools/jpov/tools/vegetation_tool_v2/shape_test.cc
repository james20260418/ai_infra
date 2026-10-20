// 植物拓扑结构的单元测试：校验规则 + 派生计算。
//
// 这些用例刻意做成"改一个字段就必须 FAIL"的负向验证——只测"合法资产能通过"是不够的，
// 因为校验函数最容易犯的错是**恒真**（规则写了但永远不触发）。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {
namespace {

constexpr double kPi = 3.14159265358979323846;

int g_failures = 0;

void Expect(bool cond, const std::string& what) {
  if (!cond) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// 单位圆截面（12 点，逆时针）。
CrossSection UnitCircle(int n) {
  CrossSection cs;
  cs.seam_index = 0;
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * kPi * i / n;
    cs.points.push_back(ProfilePoint{std::cos(a), std::sin(a)});
  }
  return cs;
}

// 最小合法资产：主干 + 一根子枝 + 一类叶 + 一张卡。
Shape MinimalShape() {
  Shape shape;
  // 给一句非空备注：往返测试要能证明 description 真的被搬运，而不是两边都空。
  shape.description = "柳树，垂枝。leaf_type 0 适合 leafset_A / leafset_C。";
  shape.profile = UnitCircle(12);

  Branch root;
  root.id = 0;
  root.parent = -1;
  root.level = 0;
  root.ring_count = 12;
  root.samples.push_back(BranchSample{Vec3d{0, 0, 0.0}, 0.18, 0.0});
  root.samples.push_back(BranchSample{Vec3d{0, 0, 2.0}, 0.12, 0.0});

  Branch child;
  child.id = 1;
  child.parent = 0;
  child.level = 1;
  child.ring_count = 8;
  child.samples.push_back(BranchSample{Vec3d{0, 0, 1.8}, 0.04, 0.0});
  child.samples.push_back(BranchSample{Vec3d{0.5, 0, 2.2}, 0.01, 0.0});

  shape.branches.push_back(root);
  shape.branches.push_back(child);

  LeafType type;
  type.type = 0;
  type.description = "柳叶簇";
  type.anchor_mode = "bottom_center";
  type.preferred_aspect = 0.6;
  shape.leaf_types.push_back(type);

  LeafCard card;
  card.anchor = Vec3d{0.2, 0.0, 2.0};
  card.up = Vec3d{0, 0, 1};
  card.normal = Vec3d{1, 0, 0};
  card.height = 0.25;
  card.type = 0;
  card.branch = 1;
  card.s = 0.5;
  shape.cards.push_back(card);

  return shape;
}

void TestValid() {
  Shape shape = MinimalShape();
  std::string err;
  // 先取结果再拼消息：C++ 不保证实参求值顺序，"Validate(...) 和 +err 谁先算"是未定义的，
  // 先拼消息会读到空的 err，让失败信息变成空白（这次就踩到了）。
  const bool ok = Validate(shape, &err);
  Expect(ok, "最小合法资产应通过，实际失败: " + err);
}

// 每个用例：改一个字段 -> 必须 FAIL。改完复原由各自局部拷贝保证。
void TestNegative() {
  {
    Shape s = MinimalShape();
    s.version = 0;
    std::string err;
    Expect(!Validate(s, &err), "version 为 0 应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[0].parent = -1;
    s.branches[1].parent = -1;  // 两根根枝
    std::string err;
    Expect(!Validate(s, &err) && err.find("根枝") != std::string::npos,
           "两根根枝应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[1].parent = 5;  // 父 id >= 自身
    std::string err;
    Expect(!Validate(s, &err), "parent 未在自身之前应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[1].id = 0;  // id 重复
    std::string err;
    Expect(!Validate(s, &err) && err.find("重复") != std::string::npos,
           "branch id 重复应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[0].samples.pop_back();  // 只剩 1 个采样点
    std::string err;
    Expect(!Validate(s, &err), "samples < 2 应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[0].samples[1].radius = 0.0;
    std::string err;
    Expect(!Validate(s, &err) && err.find("radius") != std::string::npos,
           "radius 为 0 应失败");
  }
  {
    Shape s = MinimalShape();
    s.profile.points.resize(2);
    std::string err;
    Expect(!Validate(s, &err), "截面点少于 3 应失败");
  }
  {
    Shape s = MinimalShape();
    // 顺时针 = 把顶点顺序反过来
    std::vector<ProfilePoint> rev(s.profile.points.rbegin(), s.profile.points.rend());
    s.profile.points = rev;
    std::string err;
    Expect(!Validate(s, &err), "截面顺时针应失败");
  }
  {
    Shape s = MinimalShape();
    s.profile.seam_index = 99;
    std::string err;
    Expect(!Validate(s, &err), "seam_index 越界应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].up = Vec3d{0, 0, 2};  // 非单位向量
    std::string err;
    Expect(!Validate(s, &err) && err.find("单位向量") != std::string::npos,
           "up 非单位向量应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].normal = Vec3d{0, 0, 1};  // 与 up 平行
    std::string err;
    Expect(!Validate(s, &err) && err.find("正交") != std::string::npos,
           "up 与 normal 平行应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].height = 0.0;
    std::string err;
    Expect(!Validate(s, &err), "height 为 0 应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].type = 7;
    std::string err;
    Expect(!Validate(s, &err), "card 引用不存在的 type 应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].branch = 9;
    std::string err;
    Expect(!Validate(s, &err), "card 引用不存在的 branch 应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].s = 1.5;
    std::string err;
    Expect(!Validate(s, &err), "card.s 越界应失败");
  }
}

void TestDerived() {
  CrossSection circle = UnitCircle(64);
  Expect(std::fabs(circle.Perimeter() - 2.0 * kPi) < 1e-2,
         "64 边单位圆周长应接近 2pi");

  CrossSection square;  // 单位正方形的外接轮廓（顶点在 ±1）
  square.points = {ProfilePoint{1, 1}, ProfilePoint{-1, 1},
                   ProfilePoint{-1, -1}, ProfilePoint{1, -1}};
  Expect(std::fabs(square.Perimeter() - 8.0) < 1e-9, "单位正方形周长应为 8");

  Shape shape = MinimalShape();
  Expect(shape.branches[0].EffectiveRingCount(shape.profile) == 12,
         "ring_count 显式给值时应直接用");
  shape.branches[0].ring_count = 0;
  Expect(shape.branches[0].EffectiveRingCount(shape.profile) == 12,
         "ring_count=0 时应回落到截面点数");
}

void TestProtoRoundTrip() {
  Shape shape = MinimalShape();

  // 运行时结构 -> proto -> 运行时结构，字段必须一致。
  ShapeProto proto;
  proto.set_version(shape.version);
  proto.set_description(shape.description);
  proto.mutable_profile()->set_seam_index(shape.profile.seam_index);
  for (const ProfilePoint& p : shape.profile.points) {
    auto* out = proto.mutable_profile()->add_points();
    out->set_x(p.x);
    out->set_y(p.y);
  }
  for (const Branch& b : shape.branches) {
    auto* out = proto.add_branches();
    out->set_id(b.id);
    out->set_parent(b.parent);
    out->set_level(b.level);
    out->set_ring_count(b.ring_count);
    for (const BranchSample& s : b.samples) {
      auto* bs = out->add_samples();
      bs->mutable_position()->set_x(s.position.x);
      bs->mutable_position()->set_y(s.position.y);
      bs->mutable_position()->set_z(s.position.z);
      bs->set_radius(s.radius);
      bs->set_tilt(s.tilt);
    }
  }
  for (const LeafType& t : shape.leaf_types) {
    auto* out = proto.add_leaf_types();
    out->set_type(t.type);
    out->set_description(t.description);
    out->set_anchor_mode(t.anchor_mode);
    out->set_preferred_aspect(t.preferred_aspect);
  }
  for (const LeafCard& c : shape.cards) {
    auto* out = proto.add_cards();
    out->mutable_anchor()->set_x(c.anchor.x);
    out->mutable_anchor()->set_y(c.anchor.y);
    out->mutable_anchor()->set_z(c.anchor.z);
    out->mutable_up()->set_x(c.up.x);
    out->mutable_up()->set_y(c.up.y);
    out->mutable_up()->set_z(c.up.z);
    out->mutable_normal()->set_x(c.normal.x);
    out->mutable_normal()->set_y(c.normal.y);
    out->mutable_normal()->set_z(c.normal.z);
    out->set_height(c.height);
    out->set_type(c.type);
    out->set_branch(c.branch);
    out->set_s(c.s);
  }

  const Shape back = FromProto(proto);
  std::string err;
  const bool ok = Validate(back, &err);
  Expect(ok, "往返后的资产应仍然合法: " + err);
  Expect(back.description == shape.description, "往返后 description 一致");
  Expect(back.version == shape.version, "往返后 version 一致");
  Expect(back.branches.size() == shape.branches.size(), "往返后枝数一致");
  Expect(back.branches[1].samples[1].position.x == shape.branches[1].samples[1].position.x,
         "往返后坐标一致");
  Expect(back.cards.size() == shape.cards.size(), "往返后卡数一致");
  Expect(back.leaf_types.size() == shape.leaf_types.size(), "往返后类型表一致");
  Expect(back.leaf_types[0].anchor_mode == "bottom_center", "往返后 anchor_mode 一致");
  Expect(back.cards[0].height == shape.cards[0].height, "往返后卡高度一致");
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

int main() {
  jpov::vegetation::TestValid();
  jpov::vegetation::TestNegative();
  jpov::vegetation::TestDerived();
  jpov::vegetation::TestProtoRoundTrip();
  if (jpov::vegetation::g_failures != 0) {
    std::printf("%d failure(s)\n", jpov::vegetation::g_failures);
    return 1;
  }
  std::printf("all vegetation shape tests passed\n");
  return 0;
}

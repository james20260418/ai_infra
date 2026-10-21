// 植物拓扑结构的单元测试：校验规则 + 派生计算 + proto 往返。
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

BranchSample MakeSample(double z, double radius,
                        std::vector<ProfilePoint> profile = {}) {
  BranchSample s;
  s.position = Vec3d{0.0, z, 0.0};
  s.radius = radius;
  s.tilt = 0.0;
  s.profile = std::move(profile);
  return s;
}

// 最小合法资产：主干 + 一根子枝 + 一类叶 + 一张卡。
// 截面全部留空 = 正圆，由 ring_count 决定点数。
Shape MinimalShape() {
  Shape shape;
  // 给一句非空备注：往返测试要能证明 description 真的被搬运，而不是两边都空。
  shape.description = "柳树，垂枝。leaf_type 0 适合 leafset_A / leafset_C。";

  Branch root;
  root.id = 0;
  root.parent = -1;
  root.level = 0;
  root.ring_count = 12;
  root.seam_index = 0;
  root.samples.push_back(MakeSample(0.0, 0.18));
  root.samples.push_back(MakeSample(2.0, 0.12));

  Branch child;
  child.id = 1;
  child.parent = 0;
  child.level = 1;
  child.ring_count = 8;
  child.seam_index = 0;
  child.samples.push_back(MakeSample(1.8, 0.04));
  child.samples.push_back(MakeSample(2.2, 0.01));

  shape.branches.push_back(root);
  shape.branches.push_back(child);

  LeafType type;
  type.type = 0;
  type.description = "柳叶簇";
  type.anchor_mode = "bottom_center";
  type.preferred_aspect = 0.6;
  shape.leaf_types.push_back(type);

  LeafCard card;
  card.anchor = Vec3d{0.2, 2.0, 0.0};
  card.up = Vec3d{0, 1, 0};
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
    s.branches[0].ring_count = 0;  // 又没有 profile ⇒ 无法确定环顶点数
    std::string err;
    Expect(!Validate(s, &err) && err.find("环顶点数") != std::string::npos,
           "ring_count=0 且无 profile 应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[0].seam_index = 99;
    std::string err;
    Expect(!Validate(s, &err) && err.find("seam_index") != std::string::npos,
           "seam_index 越界应失败");
  }
  {
    Shape s = MinimalShape();
    s.branches[0].samples[1].profile = UnitCircleProfile(12);  // 点数 != ring_count(12)? 相等
    s.branches[0].samples[1].profile.pop_back();              // 变 11 点，与 ring_count 不符
    std::string err;
    Expect(!Validate(s, &err) && err.find("环顶点数") != std::string::npos,
           "profile 点数与环顶点数不一致应失败");
  }
  {
    Shape s = MinimalShape();
    std::vector<ProfilePoint> pts = UnitCircleProfile(12);
    std::vector<ProfilePoint> rev(pts.rbegin(), pts.rend());  // 顺时针
    s.branches[0].samples[0].profile = rev;
    s.branches[0].samples[1].profile = rev;
    std::string err;
    Expect(!Validate(s, &err) && err.find("逆时针") != std::string::npos,
           "截面顺时针应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].up = Vec3d{0, 2, 0};  // 非单位向量
    std::string err;
    Expect(!Validate(s, &err) && err.find("单位向量") != std::string::npos,
           "up 非单位向量应失败");
  }
  {
    Shape s = MinimalShape();
    s.cards[0].normal = Vec3d{0, 1, 0};  // 与 up 平行
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
  // anchor_mode 只接受「根部在哪个角/边」这四个；曾经的 "center"（根部在图正中央）
  // 对造图的人没有意义，已删除 —— 这条防止它被悄悄加回来。
  for (const char* bad : {"center", "top_left", "bottom", ""}) {
    Shape s = MinimalShape();
    s.leaf_types[0].anchor_mode = bad;
    std::string err;
    Expect(!Validate(s, &err) && err.find("anchor_mode") != std::string::npos,
           std::string("anchor_mode \"") + bad + "\" 应失败");
  }
  for (const char* good : {"bottom_left", "bottom_center", "bottom_right", "top_center"}) {
    Shape s = MinimalShape();
    s.leaf_types[0].anchor_mode = good;
    std::string err;
    Expect(Validate(s, &err), std::string("anchor_mode \"") + good + "\" 应通过: " + err);
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
  // 64 边单位圆：周长接近 2π
  const std::vector<ProfilePoint> circle = UnitCircleProfile(64);
  Expect(std::fabs(SamplePerimeter(MakeSample(0, 1.0, circle), 0) - 2.0 * kPi) < 1e-2,
         "半径 1、profile=64 边单位圆的周长应接近 2pi");

  // 8 边单位圆：周长是 8·2·sin(π/8)，明显小于 2π（这条能区分"用真实边长"和"用 2πr"）
  const double octagon = 8.0 * 2.0 * std::sin(kPi / 8.0);
  Expect(std::fabs(SamplePerimeter(MakeSample(0, 0.5), 8) - 0.5 * octagon) < 1e-9,
         "8 边圆周长必须按真实边长算（不能用 2pi r）");

  // ring_count 显式给值时优先；=0 时回落到第一个非空 profile 的点数
  Shape shape = MinimalShape();
  Expect(shape.branches[0].EffectiveRingCount() == 12, "显式 ring_count 应直接用");
  shape.branches[0].ring_count = 0;
  shape.branches[0].samples[0].profile = UnitCircleProfile(16);
  shape.branches[0].samples[1].profile = UnitCircleProfile(16);
  Expect(shape.branches[0].EffectiveRingCount() == 16, "ring_count=0 应回落到 profile 点数");
}

void TestProtoRoundTrip() {
  Shape shape = MinimalShape();
  // 给主干一个逐点变化的截面，验证 per-sample profile 真的能被往返搬运。
  // 改第 0 个点（x=1, y=0）沿自身方向外扩到 1.2：极角仍是 0，星形判据不受影响，
  // 但形状已经不是圆了。
  shape.branches[0].samples[0].profile = UnitCircleProfile(12);
  shape.branches[0].samples[1].profile = UnitCircleProfile(12);
  shape.branches[0].samples[1].profile[0].x = 1.2;
  shape.branches[0].ring_count = 0;  // 交给 profile 决定环点数

  {
    std::string err;
    const bool ok = Validate(shape, &err);
    Expect(ok, "往返前的自造资产应合法: " + err);
  }

  // 运行时结构 -> proto
  ShapeProto proto;
  proto.set_version(shape.version);
  proto.set_description(shape.description);
  for (const Branch& b : shape.branches) {
    auto* out = proto.add_branches();
    out->set_id(b.id);
    out->set_parent(b.parent);
    out->set_level(b.level);
    out->set_ring_count(b.ring_count);
    out->set_seam_index(b.seam_index);
    for (const BranchSample& s : b.samples) {
      auto* bs = out->add_samples();
      bs->mutable_position()->set_x(s.position.x);
      bs->mutable_position()->set_y(s.position.y);
      bs->mutable_position()->set_z(s.position.z);
      bs->set_radius(s.radius);
      bs->set_tilt(s.tilt);
      for (const ProfilePoint& p : s.profile) {
        auto* pp = bs->add_profile();
        pp->set_x(p.x);
        pp->set_y(p.y);
      }
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
  Expect(back.branches[1].samples[1].position.y == shape.branches[1].samples[1].position.y,
         "往返后坐标一致");
  Expect(back.cards.size() == shape.cards.size(), "往返后卡数一致");
  Expect(back.cards[0].height == shape.cards[0].height, "往返后卡高度一致");
  Expect(back.branches[0].samples[1].profile.size() == 12, "往返后 profile 点数一致");
  Expect(std::fabs(back.branches[0].samples[1].profile[0].x - 1.2) < 1e-12,
         "往返后 profile 形状一致（逐点可变）");
  Expect(back.branches[0].seam_index == shape.branches[0].seam_index, "往返后 seam_index 一致");
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

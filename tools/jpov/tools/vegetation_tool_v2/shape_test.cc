// 植物拓扑结构的单元测试：校验规则 + 派生计算 + proto 往返。
//
// 这些用例刻意做成"改一个字段就必须 FAIL"的负向验证——只测"合法资产能通过"是不够的，
// 因为校验函数最容易犯的错是**恒真**（规则写了但永远不触发）。

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <google/protobuf/text_format.h>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"
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
  shape.leaf_types.push_back(type);

  LeafCard card;
  // anchor 必须落在所属枝条上（合同）：子枝是 x=z=0 的竖线，这里取 y=2.0 处的枝上点。
  card.anchor = Vec3d{0.0, 2.0, 0.0};
  card.up = Vec3d{0, 1, 0};
  card.normal = Vec3d{1, 0, 0};
  card.height = 0.25;
  card.width = 0.15;
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
    // 弯曲系数是「卡的 height 倍数」：1.0 是边界（弯过自己一整张卡），1.2 必须拒收。
    Shape s = MinimalShape();
    s.leaf_types[0].bend.crease = 1.0;
    std::string err;
    Expect(Validate(s, &err), "bend = 1.0 是合法边界: " + err);

    for (const double bad : {1.2, -1.2}) {
      Shape t = MinimalShape();
      t.leaf_types[0].bend.across = bad;
      std::string bad_err;
      Expect(!Validate(t, &bad_err) && bad_err.find("bend") != std::string::npos,
             "bend 超出 [-1, 1] 应失败（实际 " + std::to_string(bad) + "）");
    }
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
  {
    Shape s = MinimalShape();
    s.cards[0].width = 0.0;
    std::string err;
    Expect(!Validate(s, &err), "width 为 0 应失败");
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
  // anchor 的语义是「叶子与枝条的连接点」，离枝必须失败。
  // 负向基准：历史上导出曾把 anchor 写成「卡片中心 − up·h/2」，442 张卡全偏 12~18cm。
  {
    Shape s = MinimalShape();
    s.cards[0].anchor = Vec3d{0.2, 2.0, 0.0};   // 子枝中心线在 x=0
    std::string err;
    Expect(!Validate(s, &err) && err.find("anchor") != std::string::npos,
           "anchor 离枝 0.2m 应失败（容差只有枝半径 + 1cm）");
  }
  {
    // 容差边界内侧：子枝在 y=2.0 处半径 0.025，容差 = 0.025 + 0.01 = 0.035。
    // 偏 0.03 仍合法（叶柄插进管壁），偏 0.04 必须失败 —— 这条同时钉住「容差真的在查」。
    Shape ok = MinimalShape();
    ok.cards[0].anchor = Vec3d{0.03, 2.0, 0.0};
    std::string err;
    Expect(Validate(ok, &err), std::string("anchor 偏 3cm（容差 3.5cm）应通过: ") + err);

    Shape bad = MinimalShape();
    bad.cards[0].anchor = Vec3d{0.04, 2.0, 0.0};
    std::string err2;
    Expect(!Validate(bad, &err2), "anchor 偏 4cm（超容差 3.5cm）应失败");
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
  // 说明：原先这里用 SamplePerimeter 校验"周长按真实边长算"，该函数 2026-10-22 删除
  // （唯一调用方是测试）。周长的真实消费方是 sweep 的 tile 尺寸，那个由 sweep_test 覆盖。

  // ring_count 显式给值时优先；=0 时回落到第一个非空 profile 的点数
  Shape shape = MinimalShape();
  Expect(shape.branches[0].EffectiveRingCount() == 12, "显式 ring_count 应直接用");
  shape.branches[0].ring_count = 0;
  shape.branches[0].samples[0].profile = UnitCircleProfile(16);
  shape.branches[0].samples[1].profile = UnitCircleProfile(16);
  Expect(shape.branches[0].EffectiveRingCount() == 16, "ring_count=0 应回落到 profile 点数");
}

// 资产**文件**往返（2026-10-26 约定）：`.vegshape` 是二进制，文本只用于人工查看。
// 这里钉三件事：
//   1) SaveShapeBinary → LoadShapeFile 能原样读回（含 per-sample profile / leaf bend）；
//   2) 文本文件仍然能读（扩展名不是 .vegshape 就走 TextFormat）；
//   3) 扩展名与内容不符时**报错而不是乱读**（把 pbtxt 改名成 .vegshape 属于常见手误）。
void TestShapeFileRoundTrip() {
  Shape shape = MinimalShape();
  shape.branches[0].samples[0].profile = UnitCircleProfile(12);
  shape.branches[0].samples[1].profile = UnitCircleProfile(12);
  shape.branches[0].samples[1].profile[0].x = 1.2;
  shape.branches[0].ring_count = 0;
  shape.leaf_types[0].bend.along = -0.2;
  shape.leaf_types[0].bend.across = 0.12;
  shape.leaf_types[0].bend.crease = 0.15;
  shape.cards[0].width = 0.37;

  const std::string binary_path = "shape_file_roundtrip_test.vegshape";
  const std::string text_path = "shape_file_roundtrip_test.pbtxt";
  const std::string mismatched_path = "shape_file_roundtrip_test_mismatch.vegshape";

  {
    std::string err;
    Expect(SaveShapeBinary(shape, binary_path, &err), "二进制写盘应成功: " + err);
  }

  Shape loaded;
  {
    std::string err;
    Expect(LoadShapeFile(binary_path, &loaded, &err), "二进制读回应成功: " + err);
    Expect(loaded.branches.size() == shape.branches.size(), "二进制往返：枝数一致");
    Expect(loaded.cards.size() == shape.cards.size(), "二进制往返：卡数一致");
    Expect(loaded.branches[0].samples.size() == shape.branches[0].samples.size(),
           "二进制往返：采样点数一致");
    Expect(loaded.branches[0].samples[1].profile.size() == 12u, "二进制往返：profile 点数一致");
    Expect(std::fabs(loaded.branches[0].samples[1].profile[0].x - 1.2) < 1e-12,
           "二进制往返：profile 数值一致");
    Expect(std::fabs(loaded.leaf_types[0].bend.crease - 0.15) < 1e-12,
           "二进制往返：leaf bend 一致");
    Expect(std::fabs(loaded.cards[0].width - 0.37) < 1e-12, "二进制往返：卡宽一致");
  }

  // 文本文件（扩展名不是 .vegshape）仍然要能读 —— 人工改完的中间产物走这条路。
  {
    ShapeProto proto = ToProto(shape);
    std::string text;
    google::protobuf::TextFormat::PrintToString(proto, &text);
    FILE* file = std::fopen(text_path.c_str(), "wb");
    Expect(file != nullptr, "文本临时文件应能写");
    if (file != nullptr) {
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
    Shape from_text;
    std::string err;
    Expect(LoadShapeFile(text_path, &from_text, &err), "文本读回应成功: " + err);
    Expect(from_text.cards.size() == shape.cards.size(), "文本往返：卡数一致");
  }

  // 把文本改名成 .vegshape：按扩展名会当二进制解析 → 必须失败（而不是读出半棵树）。
  {
    std::string text;
    google::protobuf::TextFormat::PrintToString(ToProto(shape), &text);
    FILE* file = std::fopen(mismatched_path.c_str(), "wb");
    if (file != nullptr) {
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
    Shape bogus;
    std::string err;
    const bool ok = LoadShapeFile(mismatched_path, &bogus, &err);
    Expect(!ok, "扩展名与内容不符（文本内容 + .vegshape 后缀）必须报错");
    // 不只断言「失败」：失败也可能是碰巧解析出半个 proto 后被 Validate 拦。这里钉住
    // 「是二进制解析这一步失败的」——没这条，换个文本内容就可能靠别的原因通过。
    Expect(err.find("解析二进制") != std::string::npos,
           "失败原因应该是二进制解析失败，实际: " + err);
  }

  std::remove(binary_path.c_str());
  std::remove(text_path.c_str());
  std::remove(mismatched_path.c_str());
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
  // 叶面弯曲也要跨过序列化边界（漏搬的话资产「看着有 bend、跑起来是平的」）。
  shape.leaf_types[0].bend.along = -0.2;
  shape.leaf_types[0].bend.across = 0.12;
  shape.leaf_types[0].bend.crease = 0.15;

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
    out->mutable_bend()->set_along(t.bend.along);
    out->mutable_bend()->set_across(t.bend.across);
    out->mutable_bend()->set_crease(t.bend.crease);
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
    out->set_width(c.width);
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
  Expect(back.cards[0].width == shape.cards[0].width, "往返后卡宽度一致");
  Expect(back.branches[0].samples[1].profile.size() == 12, "往返后 profile 点数一致");
  Expect(std::fabs(back.branches[0].samples[1].profile[0].x - 1.2) < 1e-12,
         "往返后 profile 形状一致（逐点可变）");
  Expect(back.branches[0].seam_index == shape.branches[0].seam_index, "往返后 seam_index 一致");
  Expect(back.leaf_types[0].bend.along == shape.leaf_types[0].bend.along &&
             back.leaf_types[0].bend.across == shape.leaf_types[0].bend.across &&
             back.leaf_types[0].bend.crease == shape.leaf_types[0].bend.crease,
         "往返后 bend 三个系数一致");
}

// 文本格式**必须可逆**（2026-10-28 Danis 定：这是 bug，不是口味）。
//
// 背景：文本不是资产（资产是 `.vegshape` 二进制），但它是唯一的「人能改」的入口 ——
// `shape_convert` 把二进制转成文本，人改完再转回去。所以文本一旦丢精度，
// 「看到的数」和「资产里的数」就不是同一个数，review / 改参数都会被骗。
//
// 第一版用 `%.6g`（沿 Python 版的写法），实测 0.25691019529015585 → 0.256910，
// 读回来是另一个 double ⇒ 二进制 → 文本 → 二进制不再逐字节相同。
// 这里钉三件事：
//   1) 刻意挑「6 位有效数字装不下」的 double，文本往返后**逐位相同**；
//   2) -0 的符号不能丢（法线/手性判定用得上）；
//   3) 整条链「二进制 → 文本 → 二进制」逐字节相同。
void TestShapeTextIsLossless() {
  Shape shape = MinimalShape();
  // 这些值都是 %.6g 会砍掉的（任意 double、1/3、极小值、负零）。
  const double kAwkward[] = {0.25691019529015585, 1.0 / 3.0, 0.008363334349569713,
                             1.5e-9, -0.0, 1234567.8901234567};
  for (size_t i = 0; i < 6; ++i) {
    shape.branches[0].samples[i % 2].position.x = kAwkward[i];
    shape.branches[0].samples[i % 2].position.z = kAwkward[(i + 1) % 6];
  }
  shape.branches[0].samples[0].radius = 1.0 / 7.0;
  shape.branches[0].samples[1].tilt = 0.123456789012345;
  shape.cards[0].width = 0.31415926535897931;
  shape.cards[0].height = 2.7182818284590452;
  shape.leaf_types[0].bend.along = -0.083333333333333329;
  shape.leaf_types[0].bend.crease = 0.040000000000000001;

  // 1) 文本 → 读回：逐位相同（含 -0 的符号）。
  const std::string text_path = "shape_text_lossless_test.pbtxt";
  {
    const std::string text = gen::FormatShapeText(shape);
    FILE* file = std::fopen(text_path.c_str(), "wb");
    Expect(file != nullptr, "文本临时文件应能写");
    if (file != nullptr) {
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
  }
  Shape from_text;
  {
    std::string err;
    Expect(LoadShapeFile(text_path, &from_text, &err), "文本读回应成功: " + err);
    for (size_t i = 0; i < 2; ++i) {
      for (const auto& [got, want] :
           std::vector<std::pair<double, double>>{
               {from_text.branches[0].samples[i].position.x,
                shape.branches[0].samples[i].position.x},
               {from_text.branches[0].samples[i].position.z,
                shape.branches[0].samples[i].position.z}}) {
        Expect(got == want, "文本往返：坐标必须逐位相同（6 位有效数字会砍掉）");
      }
    }
    Expect(from_text.branches[0].samples[0].radius == shape.branches[0].samples[0].radius,
           "文本往返：半径逐位相同");
    Expect(from_text.branches[0].samples[1].tilt == shape.branches[0].samples[1].tilt,
           "文本往返：tilt 逐位相同");
    Expect(from_text.cards[0].width == shape.cards[0].width &&
               from_text.cards[0].height == shape.cards[0].height,
           "文本往返：卡宽高逐位相同");
    Expect(from_text.leaf_types[0].bend.along == shape.leaf_types[0].bend.along &&
               from_text.leaf_types[0].bend.crease == shape.leaf_types[0].bend.crease,
           "文本往返：bend 逐位相同");
    // -0 的符号：文本写成 "-0" 才不会在法线/手性上翻向。
    bool saw_negative_zero = false;
    for (const BranchSample& sample : from_text.branches[0].samples) {
      if (sample.position.x == 0.0 && std::signbit(sample.position.x)) {
        saw_negative_zero = true;
      }
    }
    Expect(saw_negative_zero, "文本往返：-0 的符号必须保留（写成 0 会翻向）");
  }

  // 2) 整条链：二进制 → 文本 → 二进制，逐字节相同。
  const std::string binary_a = "shape_text_lossless_a.vegshape";
  const std::string binary_b = "shape_text_lossless_b.vegshape";
  {
    std::string err;
    Expect(SaveShapeBinary(shape, binary_a, &err), "第一次写二进制应成功: " + err);
    Shape reloaded;
    Expect(LoadShapeFile(binary_a, &reloaded, &err), "读回二进制应成功: " + err);
    const std::string text = gen::FormatShapeText(reloaded);
    FILE* file = std::fopen(text_path.c_str(), "wb");
    if (file != nullptr) {
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
    Shape from_text2;
    Expect(LoadShapeFile(text_path, &from_text2, &err), "文本读回应成功: " + err);
    Expect(SaveShapeBinary(from_text2, binary_b, &err), "第二次写二进制应成功: " + err);
  }
  {
    std::FILE* a = std::fopen(binary_a.c_str(), "rb");
    std::FILE* b = std::fopen(binary_b.c_str(), "rb");
    Expect(a != nullptr && b != nullptr, "两次写出的文件都应存在");
    if (a != nullptr && b != nullptr) {
      std::string bytes_a;
      std::string bytes_b;
      char buffer[4096];
      size_t n = 0;
      while ((n = std::fread(buffer, 1, sizeof(buffer), a)) > 0) {
        bytes_a.append(buffer, n);
      }
      while ((n = std::fread(buffer, 1, sizeof(buffer), b)) > 0) {
        bytes_b.append(buffer, n);
      }
      std::fclose(a);
      std::fclose(b);
      Expect(bytes_a == bytes_b,
             "二进制 → 文本 → 二进制必须逐字节相同（差 " +
                 std::to_string(bytes_a.size()) + " vs " + std::to_string(bytes_b.size()) +
                 " 字节）");
    }
  }

  std::remove(text_path.c_str());
  std::remove(binary_a.c_str());
  std::remove(binary_b.c_str());
}


// 入库资产冒烟测试：`samples/*.vegshape` 每一份都必须还能读、还能过校验。
//
// 为什么需要：这 8 份 `.vegshape` 是**数据**，不是代码 —— 单元测试、gold 都只覆盖
// 代码路径（gold 只跑 willow + pine_game 两份）。剩下几份如果被误改、被文本改坏、
// 被 shape_gen 用错配置重生成，没有任何测试会红。
// 这里只钉「结构合法」（能 load、恰好一根根枝、parent < id、枝/卡非空），不钉数值 ——
// 数值由 pine_game 的 gold 钉。
//
// 路径：测试走 runfiles（$TEST_SRCDIR/__main__/…），`bazel run` 时退回仓库根。
void TestShippedSamplesLoad() {
  std::string dir;
  if (const char* srcdir = std::getenv("TEST_SRCDIR")) {
    dir = std::string(srcdir) + "/__main__/tools/jpov/tools/vegetation_tool_v2/samples";
  } else {
    // `bazel run` 的 cwd 是仓库根。
    dir = "tools/jpov/tools/vegetation_tool_v2/samples";
  }

  const char* const kNames[] = {
      "willow.vegshape", "willow_two_types.vegshape", "pine_low.vegshape",
      "oak_low.vegshape", "fir_low.vegshape", "pine_game.vegshape",
      "oak_game.vegshape", "fir_game.vegshape",
  };
  int loaded = 0;
  for (const char* name : kNames) {
    const std::string path = dir + "/" + name;
    Shape shape;
    std::string err;
    const bool ok = LoadShapeFile(path, &shape, &err);
    Expect(ok, std::string("入库资产应能读: ") + name + "（" + err + "）");
    if (!ok) {
      continue;
    }
    ++loaded;
    Expect(!shape.branches.empty(), std::string(name) + ": 枝不能为空");
    Expect(!shape.cards.empty(), std::string(name) + ": 卡不能为空");
    int roots = 0;
    bool parent_before_id = true;
    for (const Branch& branch : shape.branches) {
      if (branch.parent < 0) {
        ++roots;
      } else if (branch.parent >= branch.id) {
        parent_before_id = false;
      }
    }
    Expect(roots == 1, std::string(name) + ": 必须恰好一根根枝，实际 " + std::to_string(roots));
    Expect(parent_before_id, std::string(name) + ": parent 必须小于 id（拓扑序前提）");
  }
  Expect(loaded == 8, "8 份入库资产都应读到，实际 " + std::to_string(loaded));
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov


int main() {
  jpov::vegetation::TestValid();
  jpov::vegetation::TestNegative();
  jpov::vegetation::TestDerived();
  jpov::vegetation::TestProtoRoundTrip();
  jpov::vegetation::TestShapeFileRoundTrip();
  jpov::vegetation::TestShapeTextIsLossless();
  jpov::vegetation::TestShippedSamplesLoad();
  if (jpov::vegetation::g_failures != 0) {
    std::printf("%d failure(s)\n", jpov::vegetation::g_failures);
    return 1;
  }
  std::printf("all vegetation shape tests passed\n");
  return 0;
}

// shape_builder 的单测：用合成的 dump 钉住「父枝 / 层数 / 环数 / 叶卡姿态 / 拓扑序 / 锚点合同」。
//
// 用 poly 样条（不经过贝塞尔求值）让几何完全可预测：采样 = 原样拷贝，
// 断言可以写死数值而不是「约等于」。
//
//   主干： (0,0,0) → (0,0,4)，半径 0.2
//   侧枝： (0,0,2) → (1,0,2)，半径 0.05     ← 起点落在主干上 ⇒ 父枝 = 主干
//   叶卡： 围绕 (0.9, 0.05, 2) 的一张矩形，锚点应落在侧枝的 (1,0,2) 上

#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_builder.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"
#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

DumpSpline MakePolySpline(const std::vector<Vec3d>& points, double radius) {
  DumpSpline spline;
  spline.is_bezier = false;
  for (const Vec3d& point : points) {
    DumpSplinePoint dump_point;
    dump_point.co = point;
    dump_point.radius = radius;
    spline.points.push_back(dump_point);
  }
  return spline;
}

// 一张矩形叶卡（4 个顶点，顺序同 Blender 的 mesh 顶点顺序）。
void AddCard(SaplingDump* dump) {
  // 卡片平面：x 方向是宽度、y 方向是"从根到梢"、z 固定在枝的高度上。
  const Vec3d corners[4] = {
    Vec3d{ 0.8, -0.1, 2.0 },
    Vec3d{ 1.0, -0.1, 2.0 },
    Vec3d{ 1.0, 0.2, 2.0 },
    Vec3d{ 0.8, 0.2, 2.0 },
  };
  // UV：u 沿 x（0→1），v 沿 y（0→1）。
  const double uvs[4][2] = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 } };
  for (int k = 0; k < 4; ++k) {
    DumpLeafVertex vertex;
    vertex.position = corners[k];
    vertex.u = uvs[k][0];
    vertex.v = uvs[k][1];
    dump->leaf_vertices.push_back(vertex);
  }
}

SaplingDump MakeDump(bool child_first) {
  SaplingDump dump;
  dump.bevel_depth = 1.0;
  DumpSpline trunk = MakePolySpline({ Vec3d{ 0.0, 0.0, 0.0 }, Vec3d{ 0.0, 0.0, 4.0 } }, 0.2);
  DumpSpline branch =
      MakePolySpline({ Vec3d{ 0.0, 0.0, 2.0 }, Vec3d{ 1.0, 0.0, 2.0 } }, 0.05);
  if (child_first) {
    dump.splines.push_back(branch);
    dump.splines.push_back(trunk);
  } else {
    dump.splines.push_back(trunk);
    dump.splines.push_back(branch);
  }
  AddCard(&dump);
  return dump;
}

BuildOptions MakeOptions() {
  BuildOptions options;
  options.per_seg = 4;
  options.leaf_aspect = 0.5;
  options.anchor_mode = "bottom_left";
  options.leaf_type_desc = "测试叶";
  options.species = "test_tree";
  return options;
}

TEST(ShapeBuilderTest, BuildsParentLevelAndCard) {
  const SaplingDump dump = MakeDump(/*child_first=*/false);
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, MakeOptions(), &shape, &stats, &error)) << error;

  ASSERT_EQ(shape.branches.size(), 2u);
  EXPECT_EQ(shape.branches[0].parent, -1);
  EXPECT_EQ(shape.branches[0].level, 0u);
  EXPECT_EQ(shape.branches[0].ring_count, 16u);
  EXPECT_EQ(shape.branches[1].parent, 0);
  EXPECT_EQ(shape.branches[1].level, 1u);
  EXPECT_EQ(shape.branches[1].ring_count, 12u);

  // 采样：poly 样条原样保留；半径已乘 bevel_depth。
  ASSERT_EQ(shape.branches[0].samples.size(), 2u);
  EXPECT_DOUBLE_EQ(shape.branches[0].samples[1].position.y, 4.0);
  EXPECT_DOUBLE_EQ(shape.branches[0].samples[0].radius, 0.2);

  ASSERT_EQ(shape.cards.size(), 1u);
  const LeafCard& card = shape.cards[0];
  // 锚点 = 最近的采样点 = 侧枝末端 (1,0,2)（ToUp 后 y/z 互换：+Z up → +Y up）。
  EXPECT_NEAR(card.anchor.x, 1.0, 1e-12);
  EXPECT_NEAR(card.anchor.y, 2.0, 1e-12);
  EXPECT_NEAR(card.anchor.z, 0.0, 1e-12);
  // up：从离枝更近的那条对边指向另一条（Blender +Y → glTF −Z）。
  EXPECT_NEAR(card.up.x, 0.0, 1e-12);
  EXPECT_NEAR(card.up.y, 0.0, 1e-12);
  EXPECT_NEAR(card.up.z, -1.0, 1e-12);
  // normal = cross(width, up)，宽度沿 +X。
  EXPECT_NEAR(card.normal.x, 0.0, 1e-12);
  EXPECT_NEAR(card.normal.y, 1.0, 1e-12);
  EXPECT_NEAR(card.normal.z, 0.0, 1e-12);
  EXPECT_NEAR(card.height, 0.3, 1e-12);
  EXPECT_NEAR(card.width, 0.15, 1e-12);
  EXPECT_EQ(card.branch, 1u);
  EXPECT_EQ(card.type, 0u);  // UV 手性与 right 同向 ⇒ 未镜像

  ASSERT_EQ(shape.leaf_types.size(), 1u);
  EXPECT_EQ(shape.leaf_types[0].anchor_mode, "bottom_left");
  EXPECT_EQ(stats.cards, 1);
  EXPECT_EQ(stats.mirrored_cards, 0);
}

// 拓扑序：即使 dump 里子枝排在父枝前面，产物也必须满足 parent < id。
// （Python 版的「兜底」正是在这里会出错 —— 被 Validate 拒收。）
TEST(ShapeBuilderTest, ParentAlwaysPrecedesChild) {
  const SaplingDump dump = MakeDump(/*child_first=*/true);
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, MakeOptions(), &shape, &stats, &error)) << error;
  ASSERT_EQ(shape.branches.size(), 2u);
  EXPECT_EQ(shape.branches[0].parent, -1);
  EXPECT_EQ(shape.branches[1].parent, 0);
  // 叶卡挂在侧枝上，枝下标要跟着重编号。
  ASSERT_EQ(shape.cards.size(), 1u);
  EXPECT_EQ(shape.cards[0].branch, 1u);
}

// 锚点**按构造**就落在枝上（anchor := 最近的骨架采样点），所以「离枝」这条
// 合同校验在当前实现里恒成立。这里把它钉成断言：将来谁把 anchor 改成别的来源
// （例如最近线段上的插值点），这条测试会先提醒你「合同校验不再是摆设」。
TEST(ShapeBuilderTest, AnchorAlwaysLandsOnItsBranch) {
  SaplingDump dump = MakeDump(/*child_first=*/false);
  for (DumpLeafVertex& vertex : dump.leaf_vertices) {
    vertex.position.y += 1.0;  // 整张卡平移 1 m：最近枝会变，但 anchor 仍然在枝上
  }
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, MakeOptions(), &shape, &stats, &error)) << error;
  ASSERT_EQ(shape.cards.size(), 1u);
  const LeafCard& card = shape.cards[0];
  double nearest = 1e9;
  for (const BranchSample& sample : shape.branches[card.branch].samples) {
    nearest = std::min(nearest, std::sqrt((sample.position.x - card.anchor.x) *
                                              (sample.position.x - card.anchor.x) +
                                          (sample.position.y - card.anchor.y) *
                                              (sample.position.y - card.anchor.y) +
                                          (sample.position.z - card.anchor.z) *
                                              (sample.position.z - card.anchor.z)));
  }
  EXPECT_NEAR(nearest, 0.0, 1e-12);
}

// 负向：叶卡顶点数不是 4 的倍数（Blender 侧切卡逻辑坏掉时会这样）。
TEST(ShapeBuilderTest, RejectsNonQuadLeafVertices) {
  SaplingDump dump = MakeDump(/*child_first=*/false);
  dump.leaf_vertices.pop_back();
  Shape shape;
  BuildStats stats;
  std::string error;
  EXPECT_FALSE(BuildShape(dump, MakeOptions(), &shape, &stats, &error));
  EXPECT_NE(error.find("4 的倍数"), std::string::npos) << error;
}

// 文本产物：至少要有 version / description / branches / cards 这几个骨架。
TEST(ShapeBuilderTest, FormatsTextProto) {
  const SaplingDump dump = MakeDump(/*child_first=*/false);
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, MakeOptions(), &shape, &stats, &error)) << error;
  const std::string text = FormatShapeText(shape);
  EXPECT_NE(text.find("version: 1\n"), std::string::npos);
  EXPECT_NE(text.find("branches {\n"), std::string::npos);
  EXPECT_NE(text.find("position { x: 0 y: 2 z: -0 }"), std::string::npos);
  EXPECT_NE(text.find("cards {\n"), std::string::npos);
  EXPECT_NE(text.find("anchor_mode: \"bottom_left\""), std::string::npos);
}

}  // namespace
}  // namespace gen
}  // namespace vegetation
}  // namespace jpov

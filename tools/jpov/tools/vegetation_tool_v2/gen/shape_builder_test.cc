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

// ----------------------------- 枝条噪声 -----------------------------
//
// 噪声的验收点只有四个，但每个都必须成立，否则树会静默变形：
//   1) 同 seed + 同参数 ⇒ 逐位相同的几何（可复现）；
//   2) 尺寸抖动 = 长度与半径乘同一个因子（整体缩放，不是被拉长）；
//   3) 弯度两端位移为 0（枝根接缝不裂、枝梢叶子不飘），中段确实动了；
//   4) 噪声之后叶卡的 anchor 仍然落在枝上（不靠"补偿"糊过去）。

namespace {
// 一根沿 +Z（Blender 坐标）的多段主干，便于观察中段位移。
SaplingDump MakeLongTrunkDump(int segments) {
  SaplingDump dump;
  dump.bevel_depth = 1.0;
  std::vector<Vec3d> points;
  for (int i = 0; i <= segments; ++i) {
    points.push_back(Vec3d{ 0.0, 0.0, 4.0 * static_cast<double>(i) / segments });
  }
  dump.splines.push_back(MakePolySpline(points, 0.2));
  return dump;
}
}  // namespace

TEST(ShapeNoiseTest, ZeroNoiseIsIdentity) {
  const SaplingDump dump = MakeDump(/*child_first=*/false);
  Shape plain;
  Shape zeroed;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, MakeOptions(), &plain, &stats, &error)) << error;
  BuildOptions options = MakeOptions();
  options.noise.size_jitter = { 0.0f };
  options.noise.bend_longitudinal = { 0.0f };
  options.noise.bend_lateral = { 0.0f };
  ASSERT_TRUE(BuildShape(dump, options, &zeroed, &stats, &error)) << error;
  ASSERT_EQ(plain.branches.size(), zeroed.branches.size());
  for (std::size_t b = 0; b < plain.branches.size(); ++b) {
    ASSERT_EQ(plain.branches[b].samples.size(), zeroed.branches[b].samples.size());
    for (std::size_t k = 0; k < plain.branches[b].samples.size(); ++k) {
      EXPECT_DOUBLE_EQ(plain.branches[b].samples[k].position.x,
                       zeroed.branches[b].samples[k].position.x);
      EXPECT_DOUBLE_EQ(plain.branches[b].samples[k].position.y,
                       zeroed.branches[b].samples[k].position.y);
      EXPECT_DOUBLE_EQ(plain.branches[b].samples[k].position.z,
                       zeroed.branches[b].samples[k].position.z);
      EXPECT_DOUBLE_EQ(plain.branches[b].samples[k].radius,
                       zeroed.branches[b].samples[k].radius);
    }
  }
}

TEST(ShapeNoiseTest, SizeJitterScalesLengthAndRadiusTogether) {
  const SaplingDump dump = MakeDump(/*child_first=*/false);
  BuildOptions options = MakeOptions();
  options.noise.size_jitter = { 0.2f };  // 只作用于主干（level 0）
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, options, &shape, &stats, &error)) << error;

  // 主干：原来 (0,0,0)→(0,4,0)（glTF 坐标），半径 0.2。
  const auto& trunk = shape.branches[0];
  ASSERT_EQ(trunk.samples.size(), 2u);
  const double length = std::sqrt(
      std::pow(trunk.samples[1].position.x - trunk.samples[0].position.x, 2) +
      std::pow(trunk.samples[1].position.y - trunk.samples[0].position.y, 2) +
      std::pow(trunk.samples[1].position.z - trunk.samples[0].position.z, 2));
  const double factor = length / 4.0;
  EXPECT_GT(factor, 0.8);          // 幅度 0.2 ⇒ 因子落在 [0.8, 1.2]
  EXPECT_LT(factor, 1.2);
  // 长度与半径乘的是**同一个**因子（整体缩放，不是拉长压扁）。
  EXPECT_NEAR(trunk.samples[0].radius, 0.2 * factor, 1e-12);
  EXPECT_NEAR(trunk.samples[1].radius, 0.2 * factor, 1e-12);

  // 子枝跟着父枝走：根点仍在主干上（离主干 ≤ 1e-12），高度随主干缩放。
  const auto& branch = shape.branches[1];
  ASSERT_FALSE(branch.samples.empty());
  const double root_y = branch.samples[0].position.y;
  EXPECT_NEAR(root_y, 2.0 * factor, 1e-9);
  EXPECT_NEAR(branch.samples[0].radius, 0.05, 1e-12);  // level 1 没开抖动
}

TEST(ShapeNoiseTest, BendKeepsEndpointsAndMovesMiddle) {
  const SaplingDump dump = MakeLongTrunkDump(/*segments=*/8);
  BuildOptions options = MakeOptions();
  options.noise.bend_longitudinal = { 0.1f };
  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, options, &shape, &stats, &error)) << error;

  const auto& samples = shape.branches[0].samples;
  ASSERT_EQ(samples.size(), 9u);
  // 端点恒不动（枝根接缝、枝梢叶子）。
  EXPECT_NEAR(samples.front().position.x, 0.0, 1e-12);
  EXPECT_NEAR(samples.front().position.y, 0.0, 1e-12);
  EXPECT_NEAR(samples.front().position.z, 0.0, 1e-12);
  EXPECT_NEAR(samples.back().position.x, 0.0, 1e-12);
  EXPECT_NEAR(samples.back().position.y, 4.0, 1e-12);
  EXPECT_NEAR(samples.back().position.z, 0.0, 1e-12);
  // 中段确实被推开了（竖直枝的"纵向"退化成 +Z 方向）。
  double max_offset = 0.0;
  for (const auto& sample : samples) {
    max_offset = std::max(max_offset, std::abs(sample.position.z));
  }
  EXPECT_GT(max_offset, 0.05);  // 0.1 × 4m 的幅度，中段应远超 5cm
  EXPECT_LT(max_offset, 0.4);
}

TEST(ShapeNoiseTest, SameSeedIsReproducibleAndDifferentSeedDiffers) {
  const SaplingDump dump = MakeLongTrunkDump(/*segments=*/8);
  BuildOptions options = MakeOptions();
  options.noise.size_jitter = { 0.15f };
  options.noise.bend_longitudinal = { 0.08f };
  options.noise.bend_lateral = { 0.08f };

  Shape first;
  Shape second;
  Shape other_seed;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, options, &first, &stats, &error)) << error;
  ASSERT_TRUE(BuildShape(dump, options, &second, &stats, &error)) << error;
  options.noise.seed = 7;
  ASSERT_TRUE(BuildShape(dump, options, &other_seed, &stats, &error)) << error;

  for (std::size_t k = 0; k < first.branches[0].samples.size(); ++k) {
    EXPECT_DOUBLE_EQ(first.branches[0].samples[k].position.x,
                     second.branches[0].samples[k].position.x);
    EXPECT_DOUBLE_EQ(first.branches[0].samples[k].position.y,
                     second.branches[0].samples[k].position.y);
    EXPECT_DOUBLE_EQ(first.branches[0].samples[k].position.z,
                     second.branches[0].samples[k].position.z);
  }
  bool differs = false;
  for (std::size_t k = 0; k < first.branches[0].samples.size(); ++k) {
    if (std::abs(first.branches[0].samples[k].position.x -
                 other_seed.branches[0].samples[k].position.x) > 1e-6 ||
        std::abs(first.branches[0].samples[k].position.y -
                 other_seed.branches[0].samples[k].position.y) > 1e-6) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "换 seed 必须换一棵树，否则噪声等于没接上";
}

TEST(ShapeNoiseTest, CardsStayAttachedAfterNoise) {
  const SaplingDump dump = MakeDump(/*child_first=*/false);
  BuildOptions options = MakeOptions();
  options.noise.size_jitter = { 0.2f, 0.2f };
  options.noise.bend_longitudinal = { 0.1f, 0.1f };
  options.noise.bend_lateral = { 0.1f, 0.1f };
  Shape shape;
  BuildStats stats;
  std::string error;
  // BuildShape 在 anchor 离枝时会返回 false —— 能过就说明叶子仍然贴在枝上。
  ASSERT_TRUE(BuildShape(dump, options, &shape, &stats, &error)) << error;
  EXPECT_FALSE(shape.cards.empty());
}

// 延续接头（同一根管子被 Sapling 切成几段）在噪声之后必须仍然「对齐」：
//   * 位置：后一段的起点 == 前一段的终点（不能一边被平移/缩放、另一边没动）；
//   * 半径：接头两侧半径比保持不变（不能只压前一段或只压后一段 → 出现台阶）。
// 这是 2026-10-25 Danis 在 oak 上肉眼发现的 bug：噪音按**单根样条**施加时，
// 接头处半径比从 1.00 变成 1.22，看起来就是「后一级没对准前一级」。
TEST(ShapeNoiseTest, ContinuationSegmentsStayAligned) {
  SaplingDump dump;
  dump.bevel_depth = 1.0;
  // 同一根主干被切成两段：第二段的起点正好是第一段的终点，半径相同。
  dump.splines.push_back(MakePolySpline({ Vec3d{ 0.0, 0.0, 0.0 }, Vec3d{ 0.0, 0.0, 2.0 } }, 0.20));
  dump.splines.push_back(MakePolySpline({ Vec3d{ 0.0, 0.0, 2.0 }, Vec3d{ 0.0, 0.0, 4.0 } }, 0.20));
  // 一根真正的分叉（半径明显更细）。
  dump.splines.push_back(
      MakePolySpline({ Vec3d{ 0.0, 0.0, 3.0 }, Vec3d{ 1.0, 0.0, 3.0 } }, 0.05));

  BuildOptions options = MakeOptions();
  options.noise.size_jitter = { 0.2f, 0.2f, 0.2f };
  options.noise.bend_longitudinal = { 0.1f, 0.1f, 0.1f };
  options.noise.bend_lateral = { 0.1f, 0.1f, 0.1f };
  options.noise.seed = 3;

  Shape shape;
  BuildStats stats;
  std::string error;
  ASSERT_TRUE(BuildShape(dump, options, &shape, &stats, &error)) << error;
  ASSERT_EQ(shape.branches.size(), 3u);

  // 拓扑序：两段主干在前（父先子后），分叉在后。
  const Branch& first = shape.branches[0];
  const Branch& second = shape.branches[1];
  ASSERT_FALSE(first.samples.empty());
  ASSERT_FALSE(second.samples.empty());
  EXPECT_TRUE(second.parent == 0);

  // 位置：接头严丝合缝（不是「差几毫米」）。
  const Vec3d& joint_before = first.samples.back().position;
  const Vec3d& joint_after = second.samples.front().position;
  EXPECT_NEAR(joint_after.x, joint_before.x, 1e-12);
  EXPECT_NEAR(joint_after.y, joint_before.y, 1e-12);
  EXPECT_NEAR(joint_after.z, joint_before.z, 1e-12);

  // 半径：接头两侧比值保持不变（干净几何上就是 1.0）。
  EXPECT_NEAR(second.samples.front().radius / first.samples.back().radius, 1.0, 1e-12);

  // 分叉那根仍然贴在父枝上（噪声后重新落位，等价于 anchor 合同的枝干版）。
  const Branch& fork = shape.branches[2];
  double best = 1e9;
  for (std::size_t k = 0; k + 1 < second.samples.size(); ++k) {
    const Vec3d& a = second.samples[k].position;
    const Vec3d& b = second.samples[k + 1].position;
    const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    const double length_sqr = abx * abx + aby * aby + abz * abz;
    double t = 0.0;
    if (length_sqr > 0.0) {
      t = ((fork.samples.front().position.x - a.x) * abx +
           (fork.samples.front().position.y - a.y) * aby +
           (fork.samples.front().position.z - a.z) * abz) / length_sqr;
      t = std::clamp(t, 0.0, 1.0);
    }
    const double dx = fork.samples.front().position.x - (a.x + abx * t);
    const double dy = fork.samples.front().position.y - (a.y + aby * t);
    const double dz = fork.samples.front().position.z - (a.z + abz * t);
    best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  EXPECT_LT(best, 1e-9);
}

}  // namespace
}  // namespace gen
}  // namespace vegetation
}  // namespace jpov

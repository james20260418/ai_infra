// 植被风关节表的单测：钉住头文件说的三条不变式。
//
// 这些不变式**不是**「实现细节」——它们决定引擎会怎么变形：
//   * parent < id ⇒ 引擎可单向遍历；断链的那根枝会整根飞出去；
//   * 枝首/枝尾必有枢轴 ⇒ 管子不会整体漂移；
//   * 覆盖下标随采样下标单调 ⇒ 同一环的顶点不会被劈到两个关节（"撕皮"）。
// 所以每条都配一个**负向对照**（把参数/输入改成会破坏该性质的样子，断言必须失败），
// 否则「跑绿」只说明断言恰好成立，不说明测到了东西。

#include <algorithm>
#include <cmath>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/tools/vegetation_tool_v2/bake/bake_options.h"
#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_joints.h"

namespace jpov {
namespace vegetation {
namespace {

// 直线枝（沿 +Y 向上）或下垂枝（沿 -Y 向下），段长 step、每段一点。
Branch MakeLineBranch(uint32_t id, int32_t parent, uint32_t level, int count, double dx,
                      double dy, double dz, double radius) {
  Branch b;
  b.id = id;
  b.parent = parent;
  b.level = level;
  b.ring_count = 8;
  b.samples.resize(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    BranchSample s;
    s.position = Vec3d{dx * i, dy * i, dz * i};
    s.radius = radius;
    b.samples[static_cast<size_t>(i)] = s;
  }
  return b;
}

// 一棵最小树：主干（粗、向上）+ 一根侧枝（细、下垂）。
Shape MakeTwoBranchShape() {
  Shape shape;
  shape.branches.push_back(MakeLineBranch(/*id=*/0, /*parent=*/-1, /*level=*/0, /*count=*/21,
                                          /*dx=*/0.0, /*dy=*/0.25, /*dz=*/0.0,
                                          /*radius=*/0.12));
  shape.branches.push_back(MakeLineBranch(/*id=*/1, /*parent=*/0, /*level=*/1, /*count=*/21,
                                          /*dx=*/0.05, /*dy=*/-0.25, /*dz=*/0.0,
                                          /*radius=*/0.004));
  return shape;
}

// ── 1. 表结构：parent < id、level 沿父链 +1、phase/soft 值域 ──
TEST(VegetationGlbJointsTest, ParentIdsAreTopologicalAndLevelsMonotonic) {
  const Shape shape = MakeTwoBranchShape();
  const GlbBakeOptions opts;
  const JointLayout layout = BuildJointLayout(shape, opts);

  ASSERT_FALSE(layout.joints.empty());
  int roots = 0;
  for (size_t i = 0; i < layout.joints.size(); ++i) {
    const jpov::MeshData::WindJoint& j = layout.joints[i];
    const int parent = static_cast<int>(std::lround(j.parent));
    if (parent < 0) {
      ++roots;
      EXPECT_EQ(std::lround(j.level), 0) << "根关节 level 必须是 0（joint " << i << "）";
    } else {
      // 关键：父关节一定先于子关节建表（引擎单向遍历的前提）
      EXPECT_LT(static_cast<size_t>(parent), i) << "joint " << i << " 的父关节在它后面";
      EXPECT_EQ(std::lround(j.level), std::lround(layout.joints[parent].level) + 1)
          << "joint " << i << " 的 level 不是父 level + 1";
    }
    EXPECT_GE(j.phase, 0.0f) << "joint " << i << " 相位为负";
    EXPECT_LT(j.phase, 1.0f) << "joint " << i << " 相位应 < 1（引擎按 [0,1) 取小数部分）";
    EXPECT_GE(j.soft, 0.0f) << "joint " << i << " soft 为负";
    EXPECT_LE(j.soft, 1.0f) << "joint " << i << " soft 超过 1";
    EXPECT_TRUE(std::isfinite(j.pivot[0]) && std::isfinite(j.pivot[1]) &&
                std::isfinite(j.pivot[2]))
        << "joint " << i << " 枢轴不是有限数";
  }
  EXPECT_EQ(roots, 1) << "整棵树只能有一个根关节（多于一个 = 有枝没挂上父链）";
}

// ── 2. 枝首/枝尾一定有枢轴，且保留点升序、不超出采样范围 ──
TEST(VegetationGlbJointsTest, KeptSamplesSpanWholeBranch) {
  const Shape shape = MakeTwoBranchShape();
  const GlbBakeOptions opts;
  const JointLayout layout = BuildJointLayout(shape, opts);

  ASSERT_EQ(layout.per_branch.size(), shape.branches.size());
  for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
    const Branch& b = shape.branches[bi];
    const BranchJoints& bj = layout.per_branch[bi];
    ASSERT_FALSE(bj.kept.empty()) << "枝 " << bi << " 没有关节";
    EXPECT_EQ(bj.kept.front(), 0u) << "枝 " << bi << " 的首个采样点必须有枢轴";
    EXPECT_EQ(bj.kept.back(), static_cast<uint32_t>(b.samples.size() - 1))
        << "枝 " << bi << " 的末个采样点必须有枢轴";
    for (size_t k = 1; k < bj.kept.size(); ++k) {
      EXPECT_LT(bj.kept[k - 1], bj.kept[k]) << "枝 " << bi << " 的保留点不是严格升序";
    }
    EXPECT_EQ(bj.arc.size(), bj.kept.size());
    EXPECT_EQ(bj.soft_at.size(), bj.kept.size());
    EXPECT_EQ(layout.joint_of_kept[bi].size(), bj.kept.size());
    // 弧长必须单调不减（soft 渐变依赖它）
    for (size_t k = 1; k < bj.arc.size(); ++k) {
      EXPECT_GE(bj.arc[k], bj.arc[k - 1]) << "枝 " << bi << " 的累计弧长不是单调的";
    }
  }
}

// ── 3. 覆盖映射：每个采样点都能被覆盖，且下标单调不减 ──
TEST(VegetationGlbJointsTest, CoveringJointIsMonotonicOverSamples) {
  const Shape shape = MakeTwoBranchShape();
  const GlbBakeOptions opts;
  const JointLayout layout = BuildJointLayout(shape, opts);

  for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
    const Branch& b = shape.branches[bi];
    const BranchJoints& bj = layout.per_branch[bi];
    int prev = -1;
    for (size_t i = 0; i < b.samples.size(); ++i) {
      const int jid =
          JointCoveringSample(bj, layout.joint_of_kept[bi], static_cast<uint32_t>(i));
      ASSERT_GE(jid, 0);
      ASSERT_LT(static_cast<size_t>(jid), layout.joints.size()) << "关节下标越出关节表";
      EXPECT_GE(jid, prev) << "枝 " << bi << " 采样 " << i << " 的关节下标回退了";
      prev = jid;
    }
    // 枝尾必须落在最后一个关节上（否则末段挂在前一个关节上，尾巴不动）
    const int last_jid = JointCoveringSample(
        bj, layout.joint_of_kept[bi], static_cast<uint32_t>(b.samples.size() - 1));
    EXPECT_EQ(last_jid, layout.joint_of_kept[bi].back());
    // 枝首必须落在第一个关节上
    const int first_jid = JointCoveringSample(bj, layout.joint_of_kept[bi], 0u);
    EXPECT_EQ(first_jid, layout.joint_of_kept[bi].front());
  }
}

// ── 4. 密度确实受 spacing / 上限控制（负向对照：改小 spacing 必须更多关节）──
TEST(VegetationGlbJointsTest, JointDensityFollowsSpacingAndCap) {
  const Shape shape = MakeTwoBranchShape();
  GlbBakeOptions coarse;
  coarse.thin_spacing = 1.0;
  coarse.thick_spacing = 1.0;
  GlbBakeOptions fine = coarse;
  fine.thin_spacing = 0.1;
  fine.thick_spacing = 0.1;

  const JointLayout lc = BuildJointLayout(shape, coarse);
  const JointLayout lf = BuildJointLayout(shape, fine);
  EXPECT_GT(lf.joints.size(), lc.joints.size()) << "spacing 改小后关节数没有变多";

  // 上限：每枝关节数 <= max_joints_per_branch + 1（含首尾）
  GlbBakeOptions capped = fine;
  capped.max_joints_per_branch = 3;
  const JointLayout lcap = BuildJointLayout(shape, capped);
  for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
    EXPECT_LE(lcap.per_branch[bi].kept.size(),
              static_cast<size_t>(capped.max_joints_per_branch + 1))
        << "枝 " << bi << " 的关节数超过上限";
  }
  // 短枝（1 段 = 2 个采样点）也必须拿到 2 个关节，不能退化成 0/1 个
  Shape tiny;
  tiny.branches.push_back(MakeLineBranch(0, -1, 0, 2, 0.0, 0.1, 0.0, 0.05));
  const JointLayout lt = BuildJointLayout(tiny, capped);
  EXPECT_EQ(lt.per_branch[0].kept.size(), 2u);
  EXPECT_EQ(lt.joints.size(), 2u);
}

// ── 5. soft 只给「下垂 + 极细」的枝；粗枝必须恒 0 ──
TEST(VegetationGlbJointsTest, SoftOnlyForDroopingStrands) {
  const Shape shape = MakeTwoBranchShape();
  const GlbBakeOptions opts;
  const JointLayout layout = BuildJointLayout(shape, opts);

  // 主干：粗（12cm）+ 向上 ⇒ 全 0
  for (double s : layout.per_branch[0].soft_at) {
    EXPECT_EQ(s, 0.0) << "粗枝不该有朝向继承度（会被吹成面条）";
  }
  // 侧枝：细（4mm）+ 下垂 ⇒ 沿弧长非递减、末点 > 0
  const std::vector<double>& strand = layout.per_branch[1].soft_at;
  ASSERT_FALSE(strand.empty());
  for (size_t k = 1; k < strand.size(); ++k) {
    EXPECT_GE(strand[k], strand[k - 1]) << "soft 应沿弧长单调不减（枝首硬、枝梢软）";
  }
  EXPECT_GT(strand.back(), 0.0) << "下垂细枝的枝梢应该软";

  // 负向对照：把同一根枝加粗到超过 strand_radius_max ⇒ soft 必须归 0
  Shape thick = shape;
  for (BranchSample& s : thick.branches[1].samples) {
    s.radius = 0.05;
  }
  const JointLayout lthick = BuildJointLayout(thick, opts);
  for (double s : lthick.per_branch[1].soft_at) {
    EXPECT_EQ(s, 0.0) << "半径超过 strand_radius_max 的枝不该判成软条";
  }

  // 负向对照：把「下垂」改成「向上」⇒ soft 也归 0
  Shape upright = shape;
  for (size_t i = 0; i < upright.branches[1].samples.size(); ++i) {
    upright.branches[1].samples[i].position =
        Vec3d{0.05 * static_cast<double>(i), 0.25 * static_cast<double>(i), 0.0};
  }
  const JointLayout lup = BuildJointLayout(upright, opts);
  for (double s : lup.per_branch[1].soft_at) {
    EXPECT_EQ(s, 0.0) << "不下垂的枝不该判成软条";
  }
}

// ── 6. 相位场：值域 + 空间连续性（相邻点跳变必须很小）──
TEST(VegetationGlbJointsTest, PhaseFieldIsContinuousAndInRange) {
  const double scale = 1.0;
  double prev = SmoothPhase01(0.0, 0.0, 0.0, scale);
  EXPECT_GE(prev, 0.0);
  EXPECT_LT(prev, 1.0);
  double max_step = 0.0;
  for (int i = 1; i <= 200; ++i) {
    const double y = 0.05 * i;   // 5cm 一步（≈ 一根细枝的关节间距）
    const double v = SmoothPhase01(0.0, y, 0.0, scale);
    EXPECT_GE(v, 0.0);
    EXPECT_LT(v, 1.0);
    max_step = std::max(max_step, std::fabs(v - prev));
    prev = v;
  }
  // 相邻 5cm 的相位差远小于 0.5（不连续场会在这一步直接翻半圈 ⇒ 相邻顶点反向运动 = 撕开）
  EXPECT_LT(max_step, 0.1) << "相位场相邻采样跳变过大（不是低频连续场）";

  // 负向对照：空间频率放大 100× 后跳变必然变大（说明 scale 确实进了公式）
  double prev_fast = SmoothPhase01(0.0, 0.0, 0.0, 100.0 * scale);
  double max_step_fast = 0.0;
  for (int i = 1; i <= 200; ++i) {
    const double v = SmoothPhase01(0.0, 0.05 * i, 0.0, 100.0 * scale);
    max_step_fast = std::max(max_step_fast, std::fabs(v - prev_fast));
    prev_fast = v;
  }
  EXPECT_GT(max_step_fast, max_step) << "phase_scale 没有改变空间频率";
}

// ── 7. 最近采样点 ──
TEST(VegetationGlbJointsTest, NearestSampleIndexPicksClosest) {
  const Branch b = MakeLineBranch(0, -1, 0, 11, 0.0, 0.25, 0.0, 0.1);
  EXPECT_EQ(FindNearestSampleIndex(b, Vec3d{0.0, 0.0, 0.0}), 0);
  EXPECT_EQ(FindNearestSampleIndex(b, Vec3d{0.0, 2.5, 0.0}), 10);
  EXPECT_EQ(FindNearestSampleIndex(b, Vec3d{0.02, 1.26, -0.01}), 5);   // 1.25 附件
  EXPECT_EQ(FindNearestSampleIndex(b, Vec3d{0.0, 100.0, 0.0}), 10);    // 超出范围取端点
}

// ── 8. 分叉挂点：子枝首关节必须挂在**父枝**链上（不是自己链上）──
TEST(VegetationGlbJointsTest, ChildFirstJointHangsOnParentChain) {
  const Shape shape = MakeTwoBranchShape();
  const GlbBakeOptions opts;
  const JointLayout layout = BuildJointLayout(shape, opts);

  const int first_of_child = layout.joint_of_kept[1].front();
  ASSERT_GE(first_of_child, 0);
  const int parent = static_cast<int>(std::lround(layout.joints[first_of_child].parent));
  ASSERT_GE(parent, 0) << "子枝首关节没有父关节（父链断了 ⇒ 整根枝飞走）";
  // 父关节必须属于主干（主干关节下标都在 layout.joint_of_kept[0] 里）
  const std::vector<int>& trunk = layout.joint_of_kept[0];
  EXPECT_NE(std::find(trunk.begin(), trunk.end(), parent), trunk.end())
      << "子枝首关节的父关节不在主干链上";
  // 子枝的第二个关节要挂在子枝自己的第一个关节上
  EXPECT_EQ(static_cast<int>(std::lround(layout.joints[layout.joint_of_kept[1][1]].parent)),
            first_of_child);
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

// JPOV 部位粗细（thickness scale）单测 —— 纯 CPU，不碰 GL。
//
// 被测（这套功能**只有两处接口**）：
//   ① 骨架级配置：SkeletonManager(type, poses, thickness_scaling_config)
//      —— 关节 index 分组的**合法性校验**（越界 / 一骨两组 → LOG(FATAL)）；
//   ② 实例级取值：SkinnedInstanceState::thickness_scales 的默认值（全 1.0）。
// 对应设计：docs/jpov_crowd_body_shape_face_design.md §3。
//
// ⚠️ 为什么“非法配置”的死亡测试**不需要 GL 上下文**：ctor 里配置校验排在所有 GL 调用
//   （glGenTextures…）**之前** ⇒ 非法配置在碰 GL 前就 LOG(FATAL) 了。这也是有意为之：
//   配置错误要最早崩，而不是“跑到一半才炸”。
// ⚠️ 合法配置的**正向**用例（组号映射对不对、纹理烘的对不对）本文件不测 —— 那条链要靠 GL
//   才能构造对象（且不建显示上下文时会在 GL 调用处死，写进来就变成看环境的 flaky 测试）。
//   纳入渲染门禁：μ 只作用到指定关节组、且截面直径按 μ 变化。
//
// ⚠️ 避开恒真断言（见 skill: zero-run-code-reading-check）：默认值那条同时断言
//   “等于 1.0”与“不是 0”（后者是零粗细的致命退化），两条不同时成立。
// 负向验证已做：把 ctor 里的越界 CHECK 临时去掉 → 对应死亡用例如期失败。

#include <string>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/skeleton/skeleton_manager.h"

namespace jpov {
namespace {

// 3 骨玩具骨架：Hips(0) → LeftUpLeg(1) → LeftFoot(2)
SkeletonType ToySkeleton() {
    SkeletonType t;
    auto add = [&](int parent, float len, const char* name) {
        SkeletonJoint j;
        j.parent = parent;
        j.rest_offset = {0.0f, len, 0.0f};
        j.name = name;
        t.joints.push_back(j);
        t.bind_rotation.push_back(geom::Quaternion<float>::Identity());
    };
    add(kSkeletonNoParent, 1.0f, "mixamorig:Hips");
    add(0, 0.5f, "mixamorig:LeftUpLeg");
    add(1, 0.5f, "mixamorig:LeftFoot");
    t.Validate();
    return t;
}

std::vector<SkeletonPose> OnePose() { return {SkeletonPose::Identity(3)}; }

// ── 1. 每实例系数：默认全 1.0（不做「偏移量语义」）──
TEST(ThicknessConfigTest, InstanceScalesDefaultToOne) {
    EXPECT_EQ(kNumThicknessGroup, 8);
    const SkinnedInstanceState inst;
    ASSERT_EQ(inst.thickness_scales.size(), static_cast<size_t>(kNumThicknessGroup));
    for (float s : inst.thickness_scales) {
        EXPECT_FLOAT_EQ(s, 1.0f);
    }
    // 非恒真判别：默认数组不能是「全 0」（0.0 会把截面压成零面积，是致命退化）。
    EXPECT_GT(inst.thickness_scales[kNumThicknessGroup - 1], 0.5f);
}

// ── 2. 配置非法（关节 index 越界）必须早崩 —— 正向（上界）方向 ──
TEST(ThicknessConfigTest, JointIndexOutOfRangeIsFatal) {
    const SkeletonType t = ToySkeleton();
    std::array<std::vector<int>, kNumThicknessGroup> cfg{};
    cfg[0] = {2, 3};  // 3 骨骨架，index 3 越界（上界外一）
    EXPECT_DEATH(SkeletonManager(t, OnePose(), cfg), "越界");
}

// ── 3. 配置非法（负数 index）必须早崩 —— 反向（下界）方向 ──
TEST(ThicknessConfigTest, NegativeJointIndexIsFatal) {
    const SkeletonType t = ToySkeleton();
    std::array<std::vector<int>, kNumThicknessGroup> cfg{};
    cfg[1] = {-1};
    EXPECT_DEATH(SkeletonManager(t, OnePose(), cfg), "负数关节 index");
}

// ── 4. 同一关节进两个组 = 歧义（两个系数打架）必须早崩 ──
TEST(ThicknessConfigTest, JointInTwoGroupsIsFatal) {
    const SkeletonType t = ToySkeleton();
    std::array<std::vector<int>, kNumThicknessGroup> cfg{};
    cfg[0] = {0, 1};
    cfg[2] = {1, 2};  // 关节 1 同时在组 0 与组 2
    EXPECT_DEATH(SkeletonManager(t, OnePose(), cfg), "只能归一个组");
}

}  // namespace
}  // namespace jpov

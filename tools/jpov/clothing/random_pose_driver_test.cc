// JPOV 穿衣工具 — 随机摆动姿态发生器单测（无 GL / 无窗口）
//
// 覆盖：
//   1. 确定性：同 (相位, seed) 两次调用姿态逐位相同（换 seed 则不同）。
//   2. 周期性：相位 p 与 p+1 姿态完全一致（整数频次保证无缝循环）。
//   3. 限幅：每关节旋转角 ≤ amplitude；amplitude = 0 → 全单位姿态。
//   4. 根关节恒不动；bone_count / joint_rotation 尺寸守恒。
//   5. JointWave 派生量的取值范围（单位轴 / 频次 1..3 / 初相在 [0,2π)）。

#include "tools/jpov/clothing/random_pose_driver.h"

#include <cmath>
#include <string>

#include <gtest/gtest.h>

namespace jpov {
namespace clothing {
namespace {

// 造一条 n 个关节的直链骨架（0 号根，i 的父 = i-1）。bind_rotation 留空（全恒等）。
SkeletonType MakeChainSkeleton(int n) {
    SkeletonType skel;
    for (int i = 0; i < n; ++i) {
        SkeletonJoint j;
        j.parent = (i == 0) ? kSkeletonNoParent : (i - 1);
        j.rest_offset = Vec3f(0.0f, 1.0f, 0.0f);
        j.name = "j" + std::to_string(i);
        skel.joints.push_back(j);
    }
    return skel;
}

// 单位四元数对应的旋转角（弧度）：θ = 2·atan2(|xyz|, |w|) ∈ [0, π]。
float QuatAngle(const geom::Quaternion<float>& q) {
    const float vlen = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return 2.0f * std::atan2(vlen, std::fabs(q.w));
}

// 两个四元数是否表示同一旋转（允许 q 与 -q 等价：比点积绝对值）。
bool SameRotation(const geom::Quaternion<float>& a,
                  const geom::Quaternion<float>& b) {
    return std::fabs(a.Dot(b)) > 1.0f - 1e-5f;
}

// ==================== 确定性 / 风格 ====================

TEST(RandomPoseDriverTest, DeterministicSameInputs) {
    const SkeletonType skel = MakeChainSkeleton(6);
    RandomMotionParams p;
    p.amplitude_deg = 40.0f;
    p.seed = 7;
    const SkeletonPose a = RandomPoseAt(skel, 0.37f, p);
    const SkeletonPose b = RandomPoseAt(skel, 0.37f, p);
    ASSERT_EQ(a.bone_count, b.bone_count);
    for (int j = 0; j < a.bone_count; ++j) {
        EXPECT_TRUE(SameRotation(a.joint_rotation[j], b.joint_rotation[j]))
            << "joint " << j;
    }
}

TEST(RandomPoseDriverTest, DifferentSeedChangesPose) {
    const SkeletonType skel = MakeChainSkeleton(6);
    RandomMotionParams p;
    p.amplitude_deg = 40.0f;
    p.seed = 1;
    const SkeletonPose a = RandomPoseAt(skel, 0.37f, p);
    p.seed = 2;
    const SkeletonPose b = RandomPoseAt(skel, 0.37f, p);
    // 至少一个非根关节姿态不同（否则 seed 形同虚设）。
    bool any_diff = false;
    for (int j = 1; j < a.bone_count; ++j) {
        if (!SameRotation(a.joint_rotation[j], b.joint_rotation[j])) {
            any_diff = true;
        }
    }
    EXPECT_TRUE(any_diff);
}

// ==================== 周期性（无缝循环） ====================

TEST(RandomPoseDriverTest, PeriodicInPhase) {
    const SkeletonType skel = MakeChainSkeleton(8);
    RandomMotionParams p;
    p.amplitude_deg = 60.0f;
    p.seed = 3;
    // p 与 p+1 / p+3 姿态必须逐位一致（整数频次）。
    for (float base : {0.0f, 0.21f, 0.5f, 0.999f}) {
        const SkeletonPose a = RandomPoseAt(skel, base, p);
        const SkeletonPose b = RandomPoseAt(skel, base + 1.0f, p);
        const SkeletonPose c = RandomPoseAt(skel, base + 3.0f, p);
        for (int j = 0; j < a.bone_count; ++j) {
            EXPECT_TRUE(SameRotation(a.joint_rotation[j], b.joint_rotation[j]))
                << "base " << base << " joint " << j;
            EXPECT_TRUE(SameRotation(a.joint_rotation[j], c.joint_rotation[j]))
                << "base " << base << " joint " << j;
        }
    }
}

// ==================== 限幅 / 退化 ====================

TEST(RandomPoseDriverTest, JointAngleWithinAmplitude) {
    const SkeletonType skel = MakeChainSkeleton(7);
    RandomMotionParams p;
    p.amplitude_deg = 25.0f;
    p.seed = 11;
    const float amp_rad = 25.0f * kMotionPi / 180.0f;
    // 扫多个相位，任意关节摆角都不得超过 amplitude（含 1e-4 数值余量）。
    for (int step = 0; step < 200; ++step) {
        const float phase = static_cast<float>(step) / 200.0f;
        const SkeletonPose pose = RandomPoseAt(skel, phase, p);
        for (int j = 1; j < pose.bone_count; ++j) {
            EXPECT_LE(QuatAngle(pose.joint_rotation[j]), amp_rad + 1e-4f)
                << "phase " << phase << " joint " << j;
        }
    }
}

TEST(RandomPoseDriverTest, ZeroAmplitudeIsIdentity) {
    const SkeletonType skel = MakeChainSkeleton(5);
    RandomMotionParams p;
    p.amplitude_deg = 0.0f;
    p.seed = 9;
    const SkeletonPose pose = RandomPoseAt(skel, 0.33f, p);
    for (int j = 0; j < pose.bone_count; ++j) {
        EXPECT_NEAR(QuatAngle(pose.joint_rotation[j]), 0.0f, 1e-6f) << "joint " << j;
    }
}

TEST(RandomPoseDriverTest, SomeJointActuallyMoves) {
    const SkeletonType skel = MakeChainSkeleton(6);
    RandomMotionParams p;
    p.amplitude_deg = 45.0f;
    p.seed = 4;
    // 在若干相位上，至少有一个非根关节真的摆了（避免“恒单位”的假动作）。
    bool any_moved = false;
    for (int step = 0; step < 60; ++step) {
        const float phase = static_cast<float>(step) / 60.0f;
        const SkeletonPose pose = RandomPoseAt(skel, phase, p);
        for (int j = 1; j < pose.bone_count; ++j) {
            if (QuatAngle(pose.joint_rotation[j]) > 1e-3f) {
                any_moved = true;
            }
        }
    }
    EXPECT_TRUE(any_moved);
}

// ==================== 根关节 / 尺寸守恒 ====================

TEST(RandomPoseDriverTest, RootJointNeverMoves) {
    const SkeletonType skel = MakeChainSkeleton(6);
    RandomMotionParams p;
    p.amplitude_deg = 80.0f;
    p.seed = 5;
    for (int step = 0; step < 50; ++step) {
        const float phase = static_cast<float>(step) / 50.0f;
        const SkeletonPose pose = RandomPoseAt(skel, phase, p);
        EXPECT_NEAR(QuatAngle(pose.joint_rotation[0]), 0.0f, 1e-6f);
    }
}

TEST(RandomPoseDriverTest, BoneCountPreserved) {
    RandomMotionParams p;
    p.amplitude_deg = 30.0f;
    for (int n : {1, 2, 23, 65}) {
        const SkeletonType skel = MakeChainSkeleton(n);
        const SkeletonPose pose = RandomPoseAt(skel, 0.4f, p);
        EXPECT_EQ(pose.bone_count, n);
        EXPECT_EQ(static_cast<int>(pose.joint_rotation.size()), n);
    }
    // 只有一个关节（全根）时无关节可摆，仍应返回合法全单位姿态。
    const SkeletonPose single = RandomPoseAt(MakeChainSkeleton(1), 0.4f, p);
    EXPECT_EQ(single.bone_count, 1);
    EXPECT_NEAR(QuatAngle(single.joint_rotation[0]), 0.0f, 1e-6f);
}

// ==================== JointWave 取值范围 ====================

TEST(RandomPoseDriverTest, JointWaveRanges) {
    for (int joint = 0; joint < 40; ++joint) {
        for (int seed : {0, 1, 42, 1000}) {
            const JointWave w = ComputeJointWave(joint, seed);
            const float axis_len =
                std::sqrt(w.axis.x() * w.axis.x() + w.axis.y() * w.axis.y() +
                          w.axis.z() * w.axis.z());
            EXPECT_NEAR(axis_len, 1.0f, 1e-5f);
            EXPECT_GE(w.cycles, 1);
            EXPECT_LE(w.cycles, kMotionMaxCycles);
            EXPECT_GE(w.phase_rad, 0.0f);
            EXPECT_LT(w.phase_rad, 2.0f * kMotionPi + 1e-5f);
        }
    }
}

// 派生量必须在关节之间**真的不同**（否则哈希退化成常量、全骨架统一朝向 → 假动作）。
TEST(RandomPoseDriverTest, JointWaveVariesAcrossJoints) {
    const JointWave base = ComputeJointWave(1, 0);
    bool any_diff = false;
    for (int j = 2; j < 20; ++j) {
        const JointWave w = ComputeJointWave(j, 0);
        if (std::fabs(w.axis.x() - base.axis.x()) > 1e-4f ||
            std::fabs(w.phase_rad - base.phase_rad) > 1e-4f) {
            any_diff = true;
        }
    }
    EXPECT_TRUE(any_diff);
    // 频次也要用满 1..kMotionMaxCycles 这条范围（不能退化成恒 1）。
    bool saw_one = false;
    bool saw_more = false;
    for (int j = 0; j < 60; ++j) {
        const int c = ComputeJointWave(j, 0).cycles;
        if (c == 1) {
            saw_one = true;
        }
        if (c > 1) {
            saw_more = true;
        }
    }
    EXPECT_TRUE(saw_one);
    EXPECT_TRUE(saw_more);
}

TEST(RandomPoseDriverTest, JointWaveDeterministic) {
    const JointWave a = ComputeJointWave(3, 7);
    const JointWave b = ComputeJointWave(3, 7);
    EXPECT_EQ(a.cycles, b.cycles);
    EXPECT_FLOAT_EQ(a.phase_rad, b.phase_rad);
    EXPECT_FLOAT_EQ(a.axis.x(), b.axis.x());
    EXPECT_FLOAT_EQ(a.axis.y(), b.axis.y());
    EXPECT_FLOAT_EQ(a.axis.z(), b.axis.z());
}

}  // namespace
}  // namespace clothing
}  // namespace jpov

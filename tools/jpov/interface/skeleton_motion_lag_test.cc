// JPOV 骨架动作延迟工具（skeleton_motion_lag.h）纯 CPU 单测 —— 无 GL。
//
// 覆盖：SkeletonPoseLerp（逐骨 slerp 最短路径 + root_offset 线性）、一阶追踪（10–90% 上升时间 /
//   无过冲 / root_offset 一并滞后）、二阶追踪（超调量 + 上升时间符合配置 / 临界阻尼无过冲 /
//   root_offset 一并滞后）。

#include "tools/jpov/interface/skeleton_motion_lag.h"

#include <cmath>

#include <gtest/gtest.h>

#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {
namespace {

using geom::Quaternion;
// Vec3f = jpov::Vec3f（skeleton_types.h 已在本命名空间定义）

// 单关节绕 Y 转 angleY（rad），root_offset = root 的姿态。
SkeletonPose MakePose(int bone_count, float angle_y, const Vec3f& root) {
    SkeletonPose p;
    p.bone_count = bone_count;
    p.joint_rotation.assign(static_cast<size_t>(bone_count), Quaternion<float>::Identity());
    p.joint_rotation[0] =
        Quaternion<float>::FromAxisAngle(Vec3f(0.0f, 1.0f, 0.0f), angle_y);
    p.root_offset = root;
    return p;
}

// 四元数旋转角（rad，取 [0,π]）：2·acos(|w|)。
float QuatAngle(const Quaternion<float>& q) {
    const float w = std::min(1.0f, std::max(-1.0f, std::fabs(q.w)));
    return 2.0f * std::acos(w);
}

// ── SkeletonPoseLerp：一半插值 + root_offset 线性 ──
TEST(SkeletonMotionLagTest, PoseLerpHalvesAndLerpsRootOffset) {
    const SkeletonPose a = MakePose(3, 0.0f, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose b = MakePose(3, 1.0f, Vec3f(2.0f, -4.0f, 6.0f));
    const SkeletonPose mid = SkeletonPoseLerp(a, b, 0.5f);
    EXPECT_NEAR(QuatAngle(mid.joint_rotation[0]), 0.5f, 1e-4f);
    EXPECT_NEAR(mid.root_offset.x(), 1.0f, 1e-5f);
    EXPECT_NEAR(mid.root_offset.y(), -2.0f, 1e-5f);
    EXPECT_NEAR(mid.root_offset.z(), 3.0f, 1e-5f);
    // t 越界 clamp。
    const SkeletonPose at1 = SkeletonPoseLerp(a, b, 2.0f);
    EXPECT_NEAR(QuatAngle(at1.joint_rotation[0]), 1.0f, 1e-4f);
}

// ── SkeletonPoseLerp：最短路径（+170° → −170° 应经 ±180°，而非绕过 0°）──
TEST(SkeletonMotionLagTest, PoseLerpTakesShortestPath) {
    const float d = 170.0f * 3.14159265358979323846f / 180.0f;
    const SkeletonPose a = MakePose(1, +d, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose b = MakePose(1, -d, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose mid = SkeletonPoseLerp(a, b, 0.5f);
    // 最短路径的中点角 ≈ 180°（而非 0°）。
    EXPECT_NEAR(QuatAngle(mid.joint_rotation[0]), 3.14159265f, 2e-2f);
}

// ── 一阶追踪：10–90% 上升时间 ≈ 配置值；单调无过冲；最终贴合目标 ──
TEST(SkeletonMotionLagTest, FirstOrderRiseTimeAndNoOvershoot) {
    const float rise_s = 0.10f;
    FirstOrderLagTracker tracker(rise_s);
    const SkeletonPose start = MakePose(1, 0.0f, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose target = MakePose(1, 1.0f, Vec3f(1.0f, 0.0f, 0.0f));
    tracker.Reset(start);

    const float dt = 0.0005f;
    float t10 = -1.0f;
    float t90 = -1.0f;
    float prev = 0.0f;
    for (float t = 0.0f; t <= 1.0f; t += dt) {
        tracker.Update(target, dt);
        const float ang = QuatAngle(tracker.tracked().joint_rotation[0]);
        EXPECT_LE(ang, 1.0f + 1e-4f) << "一阶不应过冲";
        EXPECT_GE(ang, prev - 1e-5f) << "一阶应单调";
        prev = ang;
        if (t10 < 0.0f && ang >= 0.1f) {
            t10 = t;
        }
        if (t10 >= 0.0f && t90 < 0.0f && ang >= 0.9f) {
            t90 = t;
        }
    }
    ASSERT_GT(t10, 0.0f);
    ASSERT_GT(t90, t10);
    EXPECT_NEAR(t90 - t10, rise_s, 0.01f) << "一阶 10–90% 上升时间应 ≈ 配置值";
    EXPECT_NEAR(QuatAngle(tracker.tracked().joint_rotation[0]), 1.0f, 1e-3f)
        << "最终应贴合目标";
    // root_offset 也一并滞后（末态贴合）。
    EXPECT_NEAR(tracker.tracked().root_offset.x(), 1.0f, 1e-3f);
}

// ── 二阶追踪：超调量 ≈ 配置值；10–90% 上升时间 ≈ 配置值 ──
TEST(SkeletonMotionLagTest, SecondOrderOvershootAndRiseTime) {
    const float rise_s = 0.12f;
    const float overshoot = 0.20f;
    SecondOrderLagTracker tracker(rise_s, overshoot);
    const SkeletonPose start = MakePose(1, 0.0f, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose target = MakePose(1, 1.0f, Vec3f(0.0f, 0.0f, 0.0f));
    tracker.Reset(start);

    const float dt = 0.0005f;
    float t10 = -1.0f;
    float t90 = -1.0f;
    float peak = 0.0f;
    for (float t = 0.0f; t <= 1.5f; t += dt) {
        tracker.Update(target, dt);
        const float ang = QuatAngle(tracker.tracked().joint_rotation[0]);
        peak = std::max(peak, ang);
        if (t10 < 0.0f && ang >= 0.1f) {
            t10 = t;
        }
        if (t10 >= 0.0f && t90 < 0.0f && ang >= 0.9f) {
            t90 = t;
        }
    }
    ASSERT_GT(t10, 0.0f);
    ASSERT_GT(t90, t10);
    EXPECT_NEAR(t90 - t10, rise_s, 0.02f) << "二阶 10–90% 上升时间应 ≈ 配置值";
    EXPECT_NEAR(peak - 1.0f, overshoot, 0.05f) << "二阶超调量应 ≈ 配置值";
}

// ── 二阶：overshoot = 0 ⇒ 临界阻尼，无过冲（单调）──
TEST(SkeletonMotionLagTest, SecondOrderCriticalNoOvershoot) {
    SecondOrderLagTracker tracker(0.15f, 0.0f);
    EXPECT_NEAR(tracker.damping_ratio(), 1.0f, 1e-3f);
    const SkeletonPose start = MakePose(1, 0.0f, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose target = MakePose(1, 1.0f, Vec3f(0.0f, 0.0f, 0.0f));
    tracker.Reset(start);
    float peak = 0.0f;
    const float dt = 0.0005f;
    for (float t = 0.0f; t <= 1.5f; t += dt) {
        tracker.Update(target, dt);
        peak = std::max(peak, QuatAngle(tracker.tracked().joint_rotation[0]));
    }
    EXPECT_LE(peak, 1.0f + 5e-3f) << "临界阻尼不应过冲";
    EXPECT_NEAR(QuatAngle(tracker.tracked().joint_rotation[0]), 1.0f, 1e-2f);
}

// ── 二阶：root_offset 一并滞后（有过冲地趋近）──
TEST(SkeletonMotionLagTest, SecondOrderTracksRootOffset) {
    SecondOrderLagTracker tracker(0.10f, 0.15f);
    const SkeletonPose start = MakePose(1, 0.0f, Vec3f(0.0f, 0.0f, 0.0f));
    const SkeletonPose target = MakePose(1, 0.0f, Vec3f(1.0f, 0.0f, 0.0f));
    tracker.Reset(start);
    const float dt = 0.0005f;
    for (float t = 0.0f; t <= 1.0f; t += dt) {
        tracker.Update(target, dt);
    }
    EXPECT_NEAR(tracker.tracked().root_offset.x(), 1.0f, 1e-3f);
}

// ── 首次 Update 等价 Reset（从目标起步，无初始冲量）──
TEST(SkeletonMotionLagTest, FirstUpdateInitializesToTarget) {
    SecondOrderLagTracker t2(0.1f, 0.2f);
    const SkeletonPose target = MakePose(1, 0.7f, Vec3f(3.0f, 0.0f, 0.0f));
    t2.Update(target, 1.0f / 60.0f);
    EXPECT_NEAR(QuatAngle(t2.tracked().joint_rotation[0]), 0.7f, 1e-5f);
    EXPECT_NEAR(t2.tracked().root_offset.x(), 3.0f, 1e-5f);
}

}  // namespace
}  // namespace jpov

// JPOV Mocap — 算法/拟合的纯 CPU 单测。
//
// 这些用例刻意不读真实资产、不碰 GL：每个断言都针对一条**可反推的数学性质**，
// 这样算法改动时失败信息能直接指向「哪个不变量破了」，而不是「图看起来不对」。

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/mocap/algo/mocap_algorithms.h"
#include "tools/jpov/mocap/algo/mocap_fit.h"
#include "tools/jpov/mocap/algo/mocap_landmarks.h"
#include "tools/jpov/mocap/algo/mocap_math.h"
#include "tools/jpov/mocap/algo/mocap_types.h"

namespace jpov {
namespace mocap {
namespace {

constexpr float kPi = 3.14159265358979323846f;

MocapTrack EmptyTrack(int frame_count, double fps = 24.0) {
    MocapTrack track;
    track.fps = fps;
    track.frames.resize(static_cast<size_t>(frame_count));
    for (int i = 0; i < frame_count; ++i) {
        track.frames[static_cast<size_t>(i)].t_seconds = static_cast<double>(i) / fps;
    }
    return track;
}

MocapTrack OneFrameTrack() {
    return EmptyTrack(1);
}

void SetWorld(TrackFrame* frame, int landmark, const Vec3f& value) {
    CHECK(frame != nullptr);
    frame->world[static_cast<size_t>(landmark)] = value;
    frame->has_world = true;
}

void Set2D(TrackFrame* frame, int landmark, float x, float y) {
    CHECK(frame != nullptr);
    frame->landmarks[static_cast<size_t>(landmark)].x_px = x;
    frame->landmarks[static_cast<size_t>(landmark)].y_px = y;
    frame->has_landmarks = true;
}

// 在 T-pose 上把给定骨的方向改成 target（body-local）；其余骨保持 rest。
DirectionSequence RestDirectionsWithOverrides(
    const SkeletonType& skeleton,
    const std::vector<std::pair<int, Vec3f>>& overrides) {
    std::vector<Quatf> rest_world(skeleton.bone_count(), Quatf::Identity());
    for (int j = 0; j < skeleton.bone_count(); ++j) {
        const Quatf bind = skeleton.bind_rotation.empty()
                               ? Quatf::Identity()
                               : skeleton.bind_rotation[static_cast<size_t>(j)];
        const int p = skeleton.joints[static_cast<size_t>(j)].parent;
        rest_world[static_cast<size_t>(j)] =
            (p == kSkeletonNoParent ? bind : rest_world[static_cast<size_t>(p)] * bind)
                .Normalized();
    }
    DirectionSequence seq;
    seq.algorithm_name = "test";
    seq.frames.resize(1);
    BoneDirectionFrame& frame = seq.frames[0];
    for (int j = 1; j < kMixamo23BoneCount; ++j) {
        frame.direction[static_cast<size_t>(j)] = geom::RotateVector(
            rest_world[static_cast<size_t>(j)], Vec3f{0.0f, 1.0f, 0.0f});
        frame.valid[static_cast<size_t>(j)] = true;
    }
    for (const auto& override_item : overrides) {
        frame.direction[static_cast<size_t>(override_item.first)] = override_item.second;
        frame.valid[static_cast<size_t>(override_item.first)] = true;
    }
    return seq;
}

float QuatAngleDeg(const Quatf& a, const Quatf& b) {
    const float dot = std::fabs(a.Normalized().Dot(b.Normalized()));
    return 2.0f * std::acos(std::min(1.0f, dot)) * 180.0f / kPi;
}

// ── 注册表：第一期至少要有 4 种算法可批量比对 ──────────────────────────────
TEST(MocapAlgorithmsTest, RegistryContainsExpectedAlgorithms) {
    const std::vector<DirectionAlgorithm>& algos = ListDirectionAlgorithms();
    ASSERT_GE(algos.size(), 4u);
    for (const char* name : {"worldz", "worldz_t", "lift2d", "lift2d_t"}) {
        EXPECT_NE(FindDirectionAlgorithm(name), nullptr) << name;
    }
    EXPECT_EQ(FindDirectionAlgorithm("does_not_exist"), nullptr);
}

// ── 零相位平滑：峰值位置不后移（普通滑动平均会滞后 (win−1)/2 帧）───────────
TEST(MocapMathTest, ZeroPhaseSmoothingDoesNotShiftPeak) {
    std::vector<float> signal(21, 0.0f);
    signal[10] = 1.0f;
    const std::vector<float> smoothed = internal::SmoothZeroPhase(signal, 5);
    ASSERT_EQ(smoothed.size(), signal.size());
    const auto peak = std::max_element(smoothed.begin(), smoothed.end());
    EXPECT_EQ(std::distance(smoothed.begin(), peak), 10);
    EXPECT_NEAR(smoothed[10], 0.29f, 0.05f);  // 正反各滤一次后的中心能量
}

// ── fit：方向 == rest ⇒ pose 恒等（T-pose 不被意外改动）────────────────────
TEST(MocapFitTest, RestDirectionsProduceIdentityPose) {
    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    const DirectionSequence dirs = RestDirectionsWithOverrides(skeleton, {});
    FitOptions options;
    options.solve_body_yaw = false;
    options.solve_root_translation = false;
    const FitSequence fit = FitFromDirections(skeleton, dirs, OneFrameTrack(), options);
    ASSERT_EQ(fit.frames.size(), 1u);
    for (int j = 1; j < kMixamo23BoneCount; ++j) {
        EXPECT_LT(QuatAngleDeg(fit.frames[0].pose.joint_rotation[static_cast<size_t>(j)],
                              Quatf::Identity()),
                  1e-3f)
            << "joint " << j;
    }
}

// ── 整体朝向：人朝 +X（yaw=+90°）时，左右髋线应转到模型 −Z ─────────────────
TEST(MocapFitTest, BodyYawIsEstimatedAndAppliedAtHips) {
    MocapTrack track = OneFrameTrack();
    TrackFrame& frame = track.frames[0];
    SetWorld(&frame, kLeftHip, Vec3f{0.0f, 0.0f, -0.2f});
    SetWorld(&frame, kRightHip, Vec3f{0.0f, 0.0f, 0.2f});
    SetWorld(&frame, kLeftShoulder, Vec3f{0.0f, 1.2f, -0.2f});
    SetWorld(&frame, kRightShoulder, Vec3f{0.0f, 1.2f, 0.2f});
    SetWorld(&frame, kNose, Vec3f{0.2f, 1.5f, 0.0f});
    EXPECT_NEAR(EstimateBodyYaw(frame), kPi * 0.5f, 1e-3f);

    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    const DirectionSequence dirs = RestDirectionsWithOverrides(skeleton, {});
    FitOptions options;
    options.solve_root_translation = false;
    options.ground_feet = false;
    const FitSequence fit = FitFromDirections(skeleton, dirs, track, options);
    const std::vector<Vec3f> positions = ForwardKinematics(skeleton, fit.frames[0].pose);
    const int left_hip = [&] {
        for (size_t i = 0; i < skeleton.joints.size(); ++i) {
            if (skeleton.joints[i].name == "mixamorig:LeftUpLeg") {
                return static_cast<int>(i);
            }
        }
        return -1;
    }();
    ASSERT_GE(left_hip, 0);
    // LeftUpLeg 的 joint 位置 = 左髋；右髋 = 另一条腿的 UpLeg。用左右大腿位置差看左右轴。
    const int right_hip = [&] {
        for (size_t i = 0; i < skeleton.joints.size(); ++i) {
            if (skeleton.joints[i].name == "mixamorig:RightUpLeg") {
                return static_cast<int>(i);
            }
        }
        return -1;
    }();
    ASSERT_GE(right_hip, 0);
    const Vec3f side = positions[static_cast<size_t>(left_hip)] -
                       positions[static_cast<size_t>(right_hip)];
    EXPECT_NEAR(side.x(), 0.0f, 0.02f);
    EXPECT_LT(side.z(), -0.10f);  // 模型 +X 被 yaw 90° 转到 −Z
}

// ── 根位移：髋中心 world 平移 0.5m ⇒ root_offset.x 跟着走 ───────────────────
TEST(MocapFitTest, HipTranslationBecomesRootOffset) {
    MocapTrack track = EmptyTrack(2);
    for (int i = 0; i < 2; ++i) {
        TrackFrame& frame = track.frames[static_cast<size_t>(i)];
        const float x = 0.5f * static_cast<float>(i);
        SetWorld(&frame, kLeftHip, Vec3f{x, 0.0f, -0.1f});
        SetWorld(&frame, kRightHip, Vec3f{x, 0.0f, 0.1f});
        SetWorld(&frame, kLeftShoulder, Vec3f{x, 1.2f, -0.1f});
        SetWorld(&frame, kRightShoulder, Vec3f{x, 1.2f, 0.1f});
        SetWorld(&frame, kNose, Vec3f{x + 0.2f, 1.5f, 0.0f});
    }
    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    DirectionSequence dirs = RestDirectionsWithOverrides(skeleton, {});
    dirs.frames.push_back(dirs.frames[0]);  // 两帧 track 对应两帧方向（第二帧同 rest）
    FitOptions options;
    options.ground_feet = false;
    options.root_smoothing_window = 1;  // 两帧 + 平滑会把位移抹掉；这里测原始映射
    const FitSequence fit = FitFromDirections(skeleton, dirs, track, options);
    ASSERT_EQ(fit.frames.size(), 2u);
    EXPECT_NEAR(fit.frames[0].pose.root_offset.x(), 0.0f, 1e-4f);
    EXPECT_NEAR(fit.frames[1].pose.root_offset.x(), 0.5f, 1e-4f);
}

// ── 脚钉地：弯膝后最低脚关节回到 y=0（这是「悬空」的直接门禁）──────────────
TEST(MocapFitTest, GroundingPutsLowestFootAtZero) {
    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    // 大腿/小腿都向前弯 40°：脚会离地，钉地必须把 root 拉下去。
    const Vec3f down_forward = Vec3f{0.0f, -std::cos(40.0f * kPi / 180.0f),
                                     std::sin(40.0f * kPi / 180.0f)};
    const DirectionSequence dirs = RestDirectionsWithOverrides(
        skeleton, {{15, down_forward}, {16, down_forward}, {19, down_forward},
                   {20, down_forward}});
    FitOptions options;
    options.solve_body_yaw = false;
    options.solve_root_translation = true;
    options.ground_feet = true;
    options.root_smoothing_window = 1;
    const FitSequence fit = FitFromDirections(skeleton, dirs, OneFrameTrack(), options);
    ASSERT_EQ(fit.frames.size(), 1u);
    const std::vector<Vec3f> positions = ForwardKinematics(skeleton, fit.frames[0].pose);
    float min_foot = 1e9f;
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        const std::string& name = skeleton.joints[i].name;
        if (name.find("Foot") != std::string::npos ||
            name.find("ToeBase") != std::string::npos) {
            min_foot = std::min(min_foot, positions[i].y());
        }
    }
    EXPECT_LT(min_foot, 0.02f);
    EXPECT_GT(min_foot, -0.02f);
    // 弯膝后脚本来在 y>0，root 必须被压低（负值）。
    EXPECT_LT(fit.frames[0].pose.root_offset.y(), 0.0f);
}

// ── lift2d 深度符号：world-z 为负 ⇒ 反解出的 z 也必须为负 ───────────────────
TEST(MocapAlgorithmsTest, Lift2DRespectsWorldZSign) {
    MocapTrack track = OneFrameTrack();
    TrackFrame& frame = track.frames[0];
    Set2D(&frame, kLeftShoulder, 100.0f, 100.0f);
    Set2D(&frame, kLeftElbow, 250.0f, 100.0f);
    SetWorld(&frame, kLeftShoulder, Vec3f{0.0f, 0.0f, 0.0f});
    SetWorld(&frame, kLeftElbow, Vec3f{0.2f, 0.0f, -0.1f});
    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    const DirectionAlgorithm* algo = FindDirectionAlgorithm("lift2d");
    ASSERT_NE(algo, nullptr);
    const DirectionSequence seq = algo->compute(track, skeleton);
    ASSERT_EQ(seq.frames.size(), 1u);
    EXPECT_TRUE(seq.frames[0].valid[8]);  // LeftArm
    EXPECT_LT(seq.frames[0].direction[8].z(), 0.0f);
}

}  // namespace
}  // namespace mocap
}  // namespace jpov

// JPOV — 动画素材时间采样（SampleClipPose）单元测试（纯 CPU / GL-free）
//
// 用手工构造的、角度可解析验证的合成 clip 覆盖采样规则的每一条（见
// interface/animation_sampler.h 文件头）：
//   1. 帧网格精确 —— t 恰落在帧上时不插值，逐值等于该帧；
//   2. 帧归属 —— t 落在 f 的哪两个整数帧之间（k = floor(f)，非 round/ceil）；
//   3. 插值方式 —— 球面插值（Slerp），不是四元数线性插值（后者在大角度下角度偏小）；
//   4. 根位移 —— root_offset 线性插值；
//   5. 循环回绕 —— 末尾帧 ↔ 首帧之间照样插值（末帧之后回绕，不是夹在末帧）；
//   6. 时间归一 —— t 超过时长 / 为负数都先按 fmod 归一到 [0, 时长)；
//   7. 输出合法 —— 骨数/尺寸保持，四元数全程单位。
//
// 断言纪律（skills/zero-run-code-reading-check）：每条断言都存在能令其失败的实现改动，
// 例如 3 用"0.25 处应为 40° 而非线性插值的 34.5°"、5 用"回绕处应 60° 而非夹在末帧的 120°"。

#include <cmath>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "geom/common/quaternion.h"
#include "tools/jpov/interface/animation_sampler.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 绕 Z 轴旋转 deg 度的单位四元数（本测试的全部旋转都取 Z 轴，便于从 w/z 反解角度）。
geom::Quaternion<float> RotZ(float deg) {
    const float half = deg * 0.5f * kPi / 180.0f;
    return geom::Quaternion<float>(0.0f, 0.0f, std::sin(half), std::cos(half));
}

// 反解纯 Z 轴旋转的带符号角度（度，(-180, 180]）：q = (0,0,sin(θ/2),cos(θ/2)) → θ=2·atan2(z,w)。
// 只对纯 Z 轴旋转单位四元数有意义；非纯 Z（x/y 非零）视为测试构造错误。
float SignedAngleZDeg(const geom::Quaternion<float>& q) {
    CHECK_LT(std::fabs(q.x), 1e-5f) << "测试构造的旋转应为纯 Z 轴";
    CHECK_LT(std::fabs(q.y), 1e-5f) << "测试构造的旋转应为纯 Z 轴";
    return 2.0f * std::atan2(q.z, q.w) * 180.0f / kPi;
}

// 合成 clip：fps=10（第 k 帧在 t = k/10），4 帧、2 骨。
//   bone[0] 角度 0/40/80/120 —— 等步 40°，用于锁"帧归属"（错一帧 → 差 40°）。
//   bone[1] 角度 0/160/160/160 —— 大步长，用于锁"插值方式"（Slerp 40° vs 线性 34.5°）。
//   root_offset.y = 0/2/2/2 —— 用于锁根位移线性插值。
//   → 循环时长 = 4/10 = 0.4s。
jpov::FBXClip MakeSyntheticClip() {
    const float kSteadyAngles[4] = {0.0f, 40.0f, 80.0f, 120.0f};
    const float kBigAngles[4] = {0.0f, 160.0f, 160.0f, 160.0f};
    const float kRootY[4] = {0.0f, 2.0f, 2.0f, 2.0f};

    jpov::FBXClip clip;
    clip.frames_per_second = 10.0;
    for (int k = 0; k < 4; ++k) {
        jpov::SkeletonPose pose;
        pose.bone_count = 2;
        pose.joint_rotation = {RotZ(kSteadyAngles[k]), RotZ(kBigAngles[k])};
        pose.root_offset = jpov::Vec3f(0.0f, kRootY[k], 0.0f);
        clip.frames.push_back(pose);
    }
    return clip;
}

// 采样并反解 bone[0]（等步骨）的带符号 Z 角（度）。
float SampleSteadyAngleZ(const jpov::FBXClip& clip, double t) {
    jpov::SkeletonPose pose;
    jpov::SampleClipPose(clip, t, &pose);
    return SignedAngleZDeg(pose.joint_rotation[0]);
}

}  // namespace

// 1a. 循环时长 = 帧数 / 帧频。
TEST(AnimationSampler, LoopDurationIsFrameCountOverFps) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    EXPECT_DOUBLE_EQ(jpov::ClipLoopDurationSeconds(clip), 0.4);  // 4 帧 / 10fps
}

// 1b. t 恰落在帧网格上 → 逐值等于该帧（不引入任何插值误差），且骨数/尺寸保持。
TEST(AnimationSampler, SampleAtGridPointEqualsThatFrame) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    for (int k = 0; k < 4; ++k) {
        jpov::SkeletonPose pose;
        EXPECT_EQ(jpov::SampleClipPose(clip, static_cast<double>(k) / 10.0, &pose), k);
        EXPECT_EQ(pose.bone_count, 2);
        ASSERT_EQ(pose.joint_rotation.size(), 2u);
        const geom::Quaternion<float>& want = clip.frames[k].joint_rotation[1];
        // 帧精确：z/w 两个分量应逐位相等（ratio==0 走拷贝路径，不做任何插值运算）。
        EXPECT_EQ(pose.joint_rotation[1].z, want.z) << "k=" << k;
        EXPECT_EQ(pose.joint_rotation[1].w, want.w) << "k=" << k;
        EXPECT_FLOAT_EQ(pose.root_offset.y(), clip.frames[k].root_offset.y());
    }
}

// 2. 帧归属：k = floor(t·fps)。0.5 帧处 = 20°（帧0↔帧1 中点），1.5 帧处 = 60°（帧1↔帧2 中点）。
//    ⚠️ 只查角度不够：用 round 选帧时 ratio 会变成负数、Slerp 反向**外插**，凑巧也能给出同一
//    角度（负向验证实测：把 floor 改成 round，仅查角度的断言全部通过）。故同时断言**返回的
//    起始帧下标 k** —— 那才是"选了哪一帧"的直接证据。
TEST(AnimationSampler, FrameElectionUsesFloor) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    jpov::SkeletonPose pose;
    EXPECT_EQ(jpov::SampleClipPose(clip, 0.5 / 10.0, &pose), 0)
        << "t=0.5 帧 应选帧0 为起点（round 会给 1）";
    EXPECT_EQ(jpov::SampleClipPose(clip, 1.5 / 10.0, &pose), 1)
        << "t=1.5 帧 应选帧1 为起点（round 会给 2）";
    EXPECT_EQ(jpov::SampleClipPose(clip, 2.999 / 10.0, &pose), 2)
        << "帧内接近下一帧仍属本帧（floor 不给 3）";

    EXPECT_NEAR(SampleSteadyAngleZ(clip, 0.5 / 10.0), 20.0f, 1e-3f)
        << "t 在帧0/帧1 之间应插到中点 20°（不是直接给帧1 的 40°）";
    EXPECT_NEAR(SampleSteadyAngleZ(clip, 1.5 / 10.0), 60.0f, 1e-3f)
        << "t 在帧1/帧2 之间应插到中点 60°（不是给帧2 的 80°）";
}

// 3. 插值方式 = 球面插值。bone[1] 从 0° 到 160°：
//    Slerp 在 0.25 处 = 40°、0.75 处 = 120°（等角速度）；
//    线性插值（nlerp）分别 ≈34.5° / 125.5° → 用 1° 容差可稳定区分。
TEST(AnimationSampler, InterpolationIsSphericalNotLinear) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    jpov::SkeletonPose pose_a;
    jpov::SampleClipPose(clip, 0.25 / 10.0, &pose_a);
    const float a = SignedAngleZDeg(pose_a.joint_rotation[1]);
    EXPECT_NEAR(a, 40.0f, 1.0f)
        << "0.25 处应为 Slerp 的 40°（线性插值会给 ≈34.5°），实测 " << a;

    jpov::SkeletonPose pose_b;
    jpov::SampleClipPose(clip, 0.75 / 10.0, &pose_b);
    const float b = SignedAngleZDeg(pose_b.joint_rotation[1]);
    EXPECT_NEAR(b, 120.0f, 1.0f)
        << "0.75 处应为 Slerp 的 120°（线性插值会给 ≈125.5°），实测 " << b;
}

// 4. root_offset 线性插值：帧0 的 y=0 → 帧1 的 y=2，0.25/0.75 处应为 0.5/1.5。
TEST(AnimationSampler, RootOffsetIsLinearlyInterpolated) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    jpov::SkeletonPose pose;
    jpov::SampleClipPose(clip, 0.25 / 10.0, &pose);
    EXPECT_NEAR(pose.root_offset.y(), 0.5f, 1e-5f);
    jpov::SampleClipPose(clip, 0.75 / 10.0, &pose);
    EXPECT_NEAR(pose.root_offset.y(), 1.5f, 1e-5f);
}

// 5. 循环回绕：t = 3.5 帧（= 0.35s，位于末帧与首帧之间）→ 末帧(120°) ↔ 首帧(0°) 的中点 = 60°。
//    若实现把末帧之后的相邻帧夹住（不取模），这里会是 120° → 失败。
TEST(AnimationSampler, LoopWrapsLastFrameToFirst) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    EXPECT_NEAR(SampleSteadyAngleZ(clip, 0.35), 60.0f, 1e-3f)
        << "末帧↔首帧之间应插值（回绕），不是夹在末帧 120°";
    jpov::SkeletonPose pose;
    jpov::SampleClipPose(clip, 0.35, &pose);
    EXPECT_NEAR(pose.root_offset.y(), 1.0f, 1e-5f) << "根位移同款回绕插值(2→0 中点)";
}

// 6. 时间归一：t = 时长 → 回到首帧；t > 时长 与 t < 0 都按 fmod 归一。
TEST(AnimationSampler, TimeIsNormalizedModuloDuration) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    EXPECT_NEAR(SampleSteadyAngleZ(clip, 0.4), 0.0f, 1e-3f) << "t=时长 应等于首帧";
    // 0.45 = 0.05 + 0.4 → 与 0.05 同姿（帧0↔帧1 的 0.5 权重 = 20°）。
    EXPECT_NEAR(SampleSteadyAngleZ(clip, 0.45), 20.0f, 1e-3f);
    // 负时间：-0.05 ≡ 0.35 → 与回绕用例同姿（60°）。缺负数分支时 floor(负数) 会取错帧 → 失败。
    EXPECT_NEAR(SampleSteadyAngleZ(clip, -0.05), 60.0f, 1e-3f)
        << "负时间应先归一到 [0,时长)";
}

// 7. 输出全程合法：骨数/尺寸保持、每骨四元数单位、root_offset 有限。
TEST(AnimationSampler, EverySampledPoseIsValid) {
    const jpov::FBXClip clip = MakeSyntheticClip();
    for (int i = 0; i <= 40; ++i) {
        const double t = -0.05 + 0.01 * static_cast<double>(i);  // 跨 0 与循环边界
        jpov::SkeletonPose pose;
        const int k = jpov::SampleClipPose(clip, t, &pose);
        ASSERT_EQ(pose.bone_count, 2) << "t=" << t;
        ASSERT_EQ(pose.joint_rotation.size(), 2u) << "t=" << t;
        EXPECT_GE(k, 0) << "t=" << t;
        EXPECT_LT(k, 4) << "t=" << t;
        for (const geom::Quaternion<float>& q : pose.joint_rotation) {
            EXPECT_NEAR(q.Norm(), 1.0f, 1e-4f) << "t=" << t << " 非单位四元数";
        }
        EXPECT_TRUE(std::isfinite(pose.root_offset.y())) << "t=" << t;
    }
}

// 8. 边界契约：空 clip / fps ≤ 0 / 帧 bone_count ≤ 0 → CHECK 崩溃（不 fallback）。
TEST(AnimationSampler, InvalidClipsCrash) {
    jpov::FBXClip empty;
    empty.frames_per_second = 10.0;
    jpov::SkeletonPose pose;
    EXPECT_DEATH(jpov::SampleClipPose(empty, 0.0, &pose), "");

    jpov::FBXClip no_fps = MakeSyntheticClip();
    no_fps.frames_per_second = 0.0;
    EXPECT_DEATH(jpov::SampleClipPose(no_fps, 0.0, &pose), "");

    jpov::FBXClip bad_bones = MakeSyntheticClip();
    bad_bones.frames[0].bone_count = 0;
    EXPECT_DEATH(jpov::SampleClipPose(bad_bones, 0.0, &pose), "");
}

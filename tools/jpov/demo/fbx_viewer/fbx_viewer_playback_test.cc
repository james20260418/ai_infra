// JPOV FBX 观察器 — 播放控制回归测试（白盒 friend / 纯 CPU，不碰 GL）
//
// 覆盖两条"需求明确、改了却看不出来"的行为（本类是 FbxViewerApp 的 friend，
// 直接调它私有的 AdvancePlayback / UpdateFramePose —— 也就是 OneIteration 每帧
// 真正走的那两个函数）：
//
//   A. 暂停语义（需求："增设一个按钮控制暂停：按下后，主频的时间停止更新"）
//      → 播放中每帧推进 1/60 秒；暂停后**不推进**；rest 模式同样不推进。
//   B. 位姿来源（需求："一个复选框两个模式"）
//      → rest 模式 = 固定 pose = identity（T-pose）；动画模式 = 按时间采样，
//        且时间推进后位姿确实随之变化（不是每帧同一张图）。
//
// 构造 App 但不 Init()（JPOV 构造函数只存配置，不碰 GL）—— 故本测试无需 DISPLAY。
//
// 断言纪律（skills/zero-run-code-reading-check）：每条断言都有能令其失败的合法实现改动
// （例：让 AdvancePlayback 忽略 paused_ → A 的暂停用例失败；让 rest 模式走采样 →
//  B 的 identity 用例失败）。

#include <cmath>

#include <glog/logging.h>

#include "geom/common/quaternion.h"
#include "tools/jpov/demo/fbx_viewer/fbx_viewer_app.h"
#include "tools/jpov/interface/animation_sampler.h"

namespace jpov_fbx_viewer {

constexpr float kPi = 3.14159265358979323846f;

// 绕 Z 轴旋转 deg 度的单位四元数（测试位姿都用 Z 轴，便于反解角度）。
geom::Quaternion<float> RotZ(float deg) {
    const float half = deg * 0.5f * kPi / 180.0f;
    return geom::Quaternion<float>(0.0f, 0.0f, std::sin(half), std::cos(half));
}

// 反解纯 Z 轴旋转角度（度，(-180,180]）。
float AngleZDeg(const geom::Quaternion<float>& q) {
    CHECK_LT(std::fabs(q.x), 1e-4f);
    CHECK_LT(std::fabs(q.y), 1e-4f);
    return 2.0f * std::atan2(q.z, q.w) * 180.0f / kPi;
}

// 合成动画：fps=10（第 k 帧在 t=k/10）、4 帧、2 骨。
//   bone[0]: 0/40/80/120°（等步 40°，用来看"时间 → 角度"是否按比例走）
//   bone[1]: 0/160/160/160°（大步，用于确认逐骨独立插值）
jpov::FBXClip MakeSyntheticClip() {
    const float kAnglesA[4] = {0.0f, 40.0f, 80.0f, 120.0f};
    const float kAnglesB[4] = {0.0f, 160.0f, 160.0f, 160.0f};
    jpov::FBXClip clip;
    clip.frames_per_second = 10.0;
    for (int k = 0; k < 4; ++k) {
        jpov::SkeletonPose pose;
        pose.bone_count = 2;
        pose.joint_rotation = {RotZ(kAnglesA[k]), RotZ(kAnglesB[k])};
        clip.frames.push_back(pose);
    }
    return clip;
}

// 合成骨架：2 骨直链（本测试不渲染，骨长/朝向取任意合法值即可）。
jpov::SkeletonType MakeSyntheticSkeleton() {
    jpov::SkeletonType skel;
    jpov::SkeletonJoint root;
    root.parent = jpov::kSkeletonNoParent;
    root.rest_offset = jpov::Vec3f(0.0f, 1.0f, 0.0f);
    root.name = "root";
    jpov::SkeletonJoint child;
    child.parent = 0;
    child.rest_offset = jpov::Vec3f(0.0f, 1.0f, 0.0f);
    child.name = "child";
    skel.joints = {root, child};
    return skel;
}

// 装好骨架 + 动画（App 不可拷贝/移动：JPOV 持有 unique_ptr Renderer，
// 故就地装配而非"造一个再返回"）。
void SetupApp(FbxViewerApp* app /*output*/) {
    CHECK(app != nullptr);
    app->skeleton_ = MakeSyntheticSkeleton();
    app->clip_ = MakeSyntheticClip();
}

constexpr double kDt = 1.0 / 60.0;  // 主频步长（OneIteration 用的就是 1/kViewerFps）

class FbxViewerPlaybackTest {
public:
    static void Run() {
        TestTimeAdvancesWhilePlaying();
        TestTimeFrozenWhenPaused();
        TestTimeFrozenInRestMode();
        TestRestModePoseIsIdentity();
        TestAnimationModePoseFollowsTime();
    }

private:
    static void ExpectTrue(bool cond, const char* msg) {
        if (!cond) {
            LOG(FATAL) << msg;
        }
    }
    static void ExpectNear(double got, double want, double tol,
                           const char* msg) {
        if (std::fabs(got - want) > tol) {
            LOG(FATAL) << msg << "（got=" << got << " want=" << want
                       << " tol=" << tol << "）";
        }
    }

    // A1. 播放中：每帧推进 1/60 秒。
    static void TestTimeAdvancesWhilePlaying() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);
        ExpectNear(app.anim_time_seconds_, 0.0, 1e-12, "初始时刻应为 0");
        app.AdvancePlaybackForTest(kDt);
        app.AdvancePlaybackForTest(kDt);
        app.AdvancePlaybackForTest(kDt);
        ExpectNear(app.anim_time_seconds_, 3.0 * kDt, 1e-9,
                   "播放中 3 帧应推进 3/60 秒");
        LOG(INFO) << "OK TestTimeAdvancesWhilePlaying";
    }

    // A2. 暂停：时间必须停住（需求原文"按下后，主频的时间停止更新"）。
    //     若实现忽略 paused_，本用例必失败。
    static void TestTimeFrozenWhenPaused() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);
        app.AdvancePlaybackForTest(kDt);
        const double t_before = app.anim_time_seconds_;
        app.paused_ = true;
        for (int i = 0; i < 30; ++i) {
            app.AdvancePlaybackForTest(kDt);
        }
        ExpectNear(app.anim_time_seconds_, t_before, 1e-12,
                   "🔴 暂停后时间不得再推进");
        // 继续播放 → 从停住的时刻接着走（不是从 0 重来、也不补跳）。
        app.paused_ = false;
        app.AdvancePlaybackForTest(kDt);
        ExpectNear(app.anim_time_seconds_, t_before + kDt, 1e-9,
                   "继续播放后应从停住处接着推进");
        LOG(INFO) << "OK TestTimeFrozenWhenPaused";
    }

    // A3. rest 模式：动画不参与显示 → 同样不推进（切回动画模式从停住处继续）。
    static void TestTimeFrozenInRestMode() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);
        app.AdvancePlaybackForTest(kDt);
        const double t_before = app.anim_time_seconds_;
        app.rest_pose_mode_ = true;
        for (int i = 0; i < 10; ++i) {
            app.AdvancePlaybackForTest(kDt);
        }
        ExpectNear(app.anim_time_seconds_, t_before, 1e-12,
                   "rest 模式下时间不得推进");
        app.rest_pose_mode_ = false;
        app.AdvancePlaybackForTest(kDt);
        ExpectNear(app.anim_time_seconds_, t_before + kDt, 1e-9,
                   "退出 rest 模式后应接着推进");
        LOG(INFO) << "OK TestTimeFrozenInRestMode";
    }

    // B1. rest 模式位姿 = identity（每骨旋转为单位四元数、旋转角 0），且不报帧号。
    static void TestRestModePoseIsIdentity() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);
        app.anim_time_seconds_ = 1.7;  // 非 0，确保 rest 模式不看时间
        app.rest_pose_mode_ = true;
        app.UpdateFramePoseForTest();
        const jpov::SkeletonPose& pose = app.frame_pose_for_test();
        ExpectTrue(pose.bone_count == 2, "rest 位姿骨数应等于骨架骨数");
        ExpectTrue(pose.joint_rotation.size() == 2u, "rest 位姿旋转数组尺寸应为骨数");
        for (const geom::Quaternion<float>& q : pose.joint_rotation) {
            ExpectNear(AngleZDeg(q), 0.0, 1e-5, "🔴 rest 模式每骨必须是 identity");
            ExpectTrue(std::fabs(std::fabs(q.w) - 1.0f) < 1e-5f,
                       "identity 四元数应形如 (0,0,0,±1)");
        }
        ExpectTrue(app.frame_index_for_test() == -1,
                   "rest 模式不应报帧号（没有取帧）");
        LOG(INFO) << "OK TestRestModePoseIsIdentity";
    }

    // B2. 动画模式：位姿 = 按时间采样（与 SampleClipPose 一致 + 帧号正确），
    //     且时间推进后位姿确实变化（防"每帧同一张图"）。
    static void TestAnimationModePoseFollowsTime() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);

        // t = 0.05s = 0.5 帧 → 起始帧 0、两帧中点：bone0 = 20°、bone1 = 80°。
        app.anim_time_seconds_ = 0.05;
        app.UpdateFramePoseForTest();
        const float a0 = AngleZDeg(app.frame_pose_for_test().joint_rotation[0]);
        const float b0 = AngleZDeg(app.frame_pose_for_test().joint_rotation[1]);
        ExpectNear(a0, 20.0, 1e-3, "t=0.5 帧处 bone0 应为两帧中点 20°");
        ExpectNear(b0, 80.0, 1e-3, "t=0.5 帧处 bone1 应为两帧中点 80°");
        ExpectTrue(app.frame_index_for_test() == 0, "t=0.05s 应报起始帧 0");

        // 与采样器直接调用结果逐骨一致（App 不许自己另算一套）。
        jpov::SkeletonPose expected;
        jpov::SampleClipPose(app.clip_, 0.05, &expected);
        for (size_t i = 0; i < expected.joint_rotation.size(); ++i) {
            ExpectTrue(std::fabs(app.frame_pose_for_test().joint_rotation[i].Dot(
                           expected.joint_rotation[i])) > 0.999999f,
                       "App 位姿必须与 SampleClipPose 一致（不得分叉）");
        }

        // 推进一帧（1/60 秒）→ t = 0.05 + 1/60 = 0.0667s = 2/3 帧；帧号仍为 0，
        // bone0 角度 = 40° × 2/3 ≈ 26.667°（比 t=0.5 帧的 20° 前进 40°/6 ≈ 6.667°）。
        app.AdvancePlaybackForTest(kDt);
        app.UpdateFramePoseForTest();
        const float a1 = AngleZDeg(app.frame_pose_for_test().joint_rotation[0]);
        ExpectNear(a1, 40.0 * ((0.05 + kDt) * 10.0), 1e-3,
                   "推进 1/60s 后 bone0 应到 40°×(2/3 帧) ≈ 26.667°");
        ExpectTrue(app.frame_index_for_test() == 0, "2/3 帧时起始帧仍为 0");
        ExpectTrue(std::fabs(a1 - a0) > 1.0f,
                   "🔴 时间推进后位姿必须变化（否则画面永远定格）");

        // 跨帧：推进到 1.5 帧处（t = 0.15s）→ 帧号 1、bone0 = 60°。
        app.anim_time_seconds_ = 0.15;
        app.UpdateFramePoseForTest();
        ExpectNear(AngleZDeg(app.frame_pose_for_test().joint_rotation[0]), 60.0,
                   1e-3, "t=1.5 帧处 bone0 应为 60°");
        ExpectTrue(app.frame_index_for_test() == 1, "t=0.15s 应报起始帧 1");
        LOG(INFO) << "OK TestAnimationModePoseFollowsTime";
    }
};

}  // namespace jpov_fbx_viewer

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    jpov_fbx_viewer::FbxViewerPlaybackTest::Run();
    LOG(INFO) << "fbx_viewer_playback_test PASSED";
    return 0;
}

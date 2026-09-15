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
#include "tools/jpov/interface/skeleton_retarget.h"

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

// 绕 X 轴旋转 deg 度的单位四元数（"异布局"测试用：造成与源不同的局部帧）。
geom::Quaternion<float> RotX(float deg) {
    const float half = deg * 0.5f * kPi / 180.0f;
    return geom::Quaternion<float>(std::sin(half), 0.0f, 0.0f, std::cos(half));
}

// 两个四元数之间的夹角（度）— 比"转了多大"用，容忍 q 与 -q。
float QuatAngleDeg(const geom::Quaternion<float>& q) {
    const float w = std::fabs(q.w);
    return 2.0f * std::acos(std::min(1.0f, w)) * 180.0f / kPi;
}

// 某骨"偏差"（相对自己 rest 转了多少）——与 skeleton_retarget.h 的定义一致：
//   deviation(j) = Rb(j)⁻¹ ⊗ W(j)
// ⚠️ 返回**四元数本身**（不是只返回角度）：只比角度会漏"轴不同"的错
//    （角度在共轭下不变，直搬与重定向可能角度相同、轴不同）。本用例第一版只比角度，
//    负向验证发现"把 QRetarget 错写成直搬"竟能通过 —— 故改成比四元数。
geom::Quaternion<float> DeviationOf(const jpov::SkeletonType& skel,
                                    const jpov::SkeletonPose& pose, int idx) {
    const std::vector<geom::Quaternion<float>> rb = jpov::RestWorldRotations(skel);
    // W(j) = W(parent) ⊗ B(j) ⊗ P(j)（一般递归，含父骨 pose）。
    const size_t n = skel.joints.size();
    std::vector<geom::Quaternion<float>> w(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> b = skel.bind_rotation.empty()
                                              ? geom::Quaternion<float>::Identity()
                                              : skel.bind_rotation[j];
        const geom::Quaternion<float> p = pose.joint_rotation.empty()
                                              ? geom::Quaternion<float>::Identity()
                                              : pose.joint_rotation[j];
        const int par = skel.joints[j].parent;
        w[j] = (par == jpov::kSkeletonNoParent)
                   ? (b * p)
                   : (w[static_cast<size_t>(par)] * b * p);
    }
    return (rb[static_cast<size_t>(idx)].Conjugate() * w[static_cast<size_t>(idx)])
        .Normalized();
}

// 两四元数的夹角（度）——容忍 q 与 -q，直接比"是否同一旋转"。
float QuatDiffDeg(const geom::Quaternion<float>& a, const geom::Quaternion<float>& b) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return 2.0f * std::acos(std::min(1.0f, std::fabs(d))) * 180.0f / kPi;
}

// 两条"偏差只比角度"的旧入口——已废弃（见 DeviationOf 的 ⚠️）。
float DeviationAngleDeg(const jpov::SkeletonType& skel, const jpov::SkeletonPose& pose,
                        int idx) {
    return QuatAngleDeg(DeviationOf(skel, pose, idx));
}

// 偏差的**物理转轴（世界系）**与给定世界轴的夹角（度）。
//   把偏差四元数的虚部（局部系的轴）乘 Rb 映到世界系，再与期望轴比。
//   用途：直搬与重定向可能"角度相同、轴不同"，此函数能把轴错的情形钉死。
float DeviationAxisWorldDeg(const jpov::SkeletonType& skel,
                            const jpov::SkeletonPose& pose, int idx,
                            const jpov::Vec3f& expect_world_axis) {
    const std::vector<geom::Quaternion<float>> rb = jpov::RestWorldRotations(skel);
    geom::Quaternion<float> d = DeviationOf(skel, pose, idx);
    if (d.w < 0.0f) {
        d = geom::Quaternion<float>(-d.x, -d.y, -d.z, -d.w);  // 规范化到 w>=0
    }
    const jpov::Vec3f local_axis(d.x, d.y, d.z);
    if (local_axis.Norm() < 1e-6f) {
        return 0.0f;  // 零角：任何轴都成立
    }
    const jpov::Vec3f world_axis = geom::RotateVector(
        rb[static_cast<size_t>(idx)], local_axis.Unit());
    float c = world_axis.x() * expect_world_axis.x() +
              world_axis.y() * expect_world_axis.y() +
              world_axis.z() * expect_world_axis.z();
    c = std::max(-1.0f, std::min(1.0f, c));
    const float deg = std::acos(c) * 180.0f / kPi;
    // ⚠️ 转轴是**带符号**的：绕 +Z 转 -30° ≡ 绕 -Z 转 +30°。两者是同一个旋转。
    //    故 180° 与 0° 在此**等价**（轴反向），一并接受；真正要排除的是**垂直/斜轴**。
    //    （第一版只接受 0°，把正确的实现误判为失败 —— 就是踩了这个等价性。）
    return std::min(deg, 180.0f - deg);
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
        TestBlueDriveModes();
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

    // ── C. 蓝骨驱动：QRetarget vs 无重定向（数值直搬）──────────────────────
    //
    // 这套用例直接验"两种驱动语义不同"——并**不靠 GL**（纯 CPU 算 glb_pose_）。
    //
    // ⚠️ **构型必须满足两个条件，否则两驱动结果相同、测不出东西**（本用例踩过两次）：
    //   ① 目标的 bind 与源**不同轴**（至少一根骨）；
    //   ② 目标的**父骨也被驱动**（父链上带偏差）。
    //   若不满足②：`P_t = B⁻¹ · W_parent⁻¹ · (Rb_t · dev)` 里的 Rb_t 与 B⁻¹ 会同轴抵消，
    //   得到 `P_t = dev = P_s` —— 两驱动**逐位相同**（实测：斜父也不动时差别 0.0°）。
    //   满足后（实际场景：髋转+腿转，两侧腿骨局部轴不同）差别显著（实测 ~83°）。
    //
    // 骨架布局：
    //   源（红）：2 骨，无 bind（全恒等）；pose = root 绕 Z 60°、child 绕 Z 30°。
    //   目标（蓝）：2 骨同名，child 的 bind 绕 X 90°；pose 由被测驱动算。
    //
    // 预期：
    //   · NoRetarget：目标 pose **数值==源 pose**（含 child 的 30°）⇒ 两驱动显著不同。
    //   · QRetarget：目标**偏差**（相对自己 bind 的增量）== 源的偏差（逐位一致）。
    static void TestBlueDriveModes() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);

        // 目标骨架同构，但 **child 的 bind 绕 X 90°**（根保持恒等）——造成"异轴"异布局。
        // ⚠️ 必须**显式填** bind_rotation：MakeSyntheticSkeleton 里是空的（= 全恒等），
        //    直接改空数组等于什么都没改（本用例第一版就这样）。
        jpov::SkeletonType& tgt = app.glb_skeleton_for_test();
        tgt = app.skeleton_for_test();
        tgt.bind_rotation.assign(tgt.joints.size(), geom::Quaternion<float>::Identity());
        tgt.bind_rotation[1] = RotX(90.0f);  // 只改 child：与源异轴
        tgt.Validate();
        app.RebuildBindForTest();

        // 对位必须命中两骨（同名），否则下面测的是"未命中保持 rest"而非驱动语义。
        ExpectTrue(app.retarget_bind_for_test().matched_bone_count == 2,
                   "异布局测试要求 2 骨全命中（否则测的不是驱动语义）");
        // 异布局确实造成了非恒等 Q（前提门禁）。值不写死：bind 沿链**复合**，
        // 具体值随布局变，写死会把"复合语义"与"用例意图"耦死。
        ExpectTrue(app.retarget_bind_for_test().q_max_angle_deg > 45.0,
                   "异布局的 Q 应明显非恒等（否则本用例测不出两驱动差异）");

        // 源 pose：**根也要转**（条件②）——根转 60°、child 转 30°。
        jpov::SkeletonPose src_pose;
        src_pose.bone_count = 2;
        src_pose.joint_rotation = {RotZ(60.0f), RotZ(30.0f)};

        // (1) NoRetarget：数值直搬 ⇒ 目标 pose **数值==源 pose**。
        app.SetBlueDriveForTest(BlueDrive::kNoRetarget);
        app.SetFramePoseForTest(src_pose);
        app.ComputeGlbPoseForTest();
        ExpectNear(AngleZDeg(app.glb_pose_for_test().joint_rotation[1]), 30.0, 1e-3,
                   "NoRetarget 应把源的 30° 数值原样写入目标 child");
        const geom::Quaternion<float> noreduce_child =
            app.glb_pose_for_test().joint_rotation[1];

        // (2) QRetarget：局部数值**应与直搬显著不同**（否则等于没做重定向）。
        app.SetBlueDriveForTest(BlueDrive::kQRetarget);
        app.ComputeGlbPoseForTest();
        const geom::Quaternion<float> qr_child =
            app.glb_pose_for_test().joint_rotation[1];
        const float drive_diff = QuatDiffDeg(noreduce_child, qr_child);
        ExpectTrue(drive_diff > 30.0f,
                   "🔴 QRetarget 与直搬的 child 位姿必须明显不同（本用例的分离点）");

        // 核心断言：偏差**四元数**一致（= 同一物理旋转）。
        // ⚠️ 必须比四元数（含轴），不能只比角度：角度在共轭下不变，
        //    "把 QRetarget 错写成直搬"时角度仍可能相同 ⇒ 只比角度会漏报（实测确实漏了）。
        const jpov::SkeletonType& src = app.skeleton_for_test();
        const jpov::SkeletonType& dst = app.glb_skeleton_for_test();
        for (int j = 0; j < 2; ++j) {
            const geom::Quaternion<float> dev_src = DeviationOf(src, src_pose, j);
            const geom::Quaternion<float> dev_dst =
                DeviationOf(dst, app.glb_pose_for_test(), j);
            ExpectNear(QuatDiffDeg(dev_dst, dev_src), 0.0, 1e-2,
                       "🔴 QRetarget 必须保持偏差（含转轴）逐骨一致");
        }
        // ⚠️ child 的"偏差"是 **Rb⁻¹W**（世界系），父骨的 60° 也在 W 里 ⇒ 偏差 = 90°
        //    （= 60° + 30°），**不是** 30°。30° 是它的**局部 pose**。
        //    （本用例第一版写 30° —— 混淆了"偏差"与"局部 pose"，被本条断言抓到。）
        ExpectNear(DeviationAngleDeg(src, src_pose, 1), 90.0, 1e-2,
                   "child 的源偏差 = 父 60° + 自 30° = 90°（世界系）");
        ExpectNear(AngleZDeg(src_pose.joint_rotation[1]), 30.0, 1e-4,
                   "（对照：child 的局部 pose 才是 30°）");

        // (3) 不变量 1：源 pose 恒等 ⇒ QRetarget 目标 pose 恒等（保持自己 rest）。
        app.SetBlueDriveForTest(BlueDrive::kQRetarget);
        app.SetFramePoseForTest(jpov::SkeletonPose::Identity(2));
        app.ComputeGlbPoseForTest();
        for (size_t j = 0; j < 2; ++j) {
            ExpectNear(QuatAngleDeg(app.glb_pose_for_test().joint_rotation[j]), 0.0, 1e-3,
                       "🔴 源不动 ⇒ QRetarget 下目标也不动（保持自己的 bind 朝向）");
        }
        LOG(INFO) << "OK TestBlueDriveModes（两驱动 child 位姿差 " << drive_diff << "°）";
    }
};

}  // namespace jpov_fbx_viewer

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    jpov_fbx_viewer::FbxViewerPlaybackTest::Run();
    LOG(INFO) << "fbx_viewer_playback_test PASSED";
    return 0;
}

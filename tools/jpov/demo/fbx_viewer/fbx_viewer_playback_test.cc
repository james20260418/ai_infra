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

// 合成动画：fps=10（第 k 帧在 t=k/10）、4 帧、**4 骨**（与 MakeSyntheticSkeleton 对应）。
//   bone[0]: 0/40/80/120°（等步 40°，用来看"时间 → 角度"是否按比例走）
//   bone[1]: 0/160/160/160°（大步，用于确认逐骨独立插值）
//   bone[2..3]: 恒等（左右手；给 BodyRetarget 的人体随动系留位）
jpov::FBXClip MakeSyntheticClip() {
    const float kAnglesA[4] = {0.0f, 40.0f, 80.0f, 120.0f};
    const float kAnglesB[4] = {0.0f, 160.0f, 160.0f, 160.0f};
    jpov::FBXClip clip;
    clip.frames_per_second = 10.0;
    for (int k = 0; k < 4; ++k) {
        jpov::SkeletonPose pose;
        pose.bone_count = 4;
        pose.joint_rotation = {RotZ(kAnglesA[k]), RotZ(kAnglesB[k]),
                               geom::Quaternion<float>::Identity(),
                               geom::Quaternion<float>::Identity()};
        clip.frames.push_back(pose);
    }
    return clip;
}

// 绕 X 轴旋转 deg 度的单位四元数（"异布局"测试用：造成与源不同的局部帧）。
geom::Quaternion<float> RotX(float deg) {
    const float half = deg * 0.5f * kPi / 180.0f;
    return geom::Quaternion<float>(std::sin(half), 0.0f, 0.0f, std::cos(half));
}

// 绕 Y 轴旋转 deg 度的单位四元数（“绕自身骨轴 roll” 测试用：对几何不可见）。
geom::Quaternion<float> RotY(float deg) {
    const float half = deg * 0.5f * kPi / 180.0f;
    return geom::Quaternion<float>(0.0f, std::sin(half), 0.0f, std::cos(half));
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

// 沿树算**全部**骨的世界总旋转（含 pose）：
//   W(j) = W(parent) ⊗ B(j) ⊗ P(j)（拓扑序单趟）。
// 与 skeleton_retarget.h 的 BodyRetarget 内部一致（这里独立复算，用于交叉校验）。
std::vector<geom::Quaternion<float>> WorldRotations(const jpov::SkeletonType& skel,
                                                    const jpov::SkeletonPose& pose) {
    const size_t n = skel.joints.size();
    std::vector<geom::Quaternion<float>> w(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> b =
            skel.bind_rotation.empty() ? geom::Quaternion<float>::Identity()
                                       : skel.bind_rotation[j];
        const geom::Quaternion<float> p =
            pose.joint_rotation.empty() ? geom::Quaternion<float>::Identity()
                                        : pose.joint_rotation[j];
        const int par = skel.joints[j].parent;
        w[j] = (par == jpov::kSkeletonNoParent)
                   ? (b * p).Normalized()
                   : (w[static_cast<size_t>(par)] * b * p).Normalized();
    }
    return w;
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
// 合成骨架：**4 骨**——root → spine → 左右手。
//   ⚠️ 必须有 Left/Right Hand（且左右分开、不沿 up）—— BodyRetarget 的人体随动系靠
//      「右腕→左腕」连线定 +X；缺了就估不出基准（LOG(FATAL)，不猜）。
//   ⚠️ 骨名用 mixamorig: 后缀可匹配形式（FindJointBySuffix 只看后缀）。
//   骨长/朝向取任意合法值（本测试不渲染）。
jpov::SkeletonType MakeSyntheticSkeleton() {
    jpov::SkeletonType skel;
    auto mk = [](int parent, float x, float y, float z, const char* n) {
        jpov::SkeletonJoint j;
        j.parent = parent;
        j.rest_offset = jpov::Vec3f(x, y, z);
        j.name = n;
        return j;
    };
    // 0 root → 1 spine → 2 右手(−X) / 3 左手(+X)。左手在 +X ⇒ 面朝 +Z。
    skel.joints = {
        mk(jpov::kSkeletonNoParent, 0.0f, 1.0f, 0.0f, "mixamorig:Hips"),
        mk(0, 0.0f, 0.2f, 0.0f, "mixamorig:Spine"),
        mk(1, -0.5f, 0.0f, 0.0f, "mixamorig:RightHand"),
        mk(1, +0.5f, 0.0f, 0.0f, "mixamorig:LeftHand"),
    };
    skel.bind_rotation.assign(skel.joints.size(), geom::Quaternion<float>::Identity());
    skel.Validate();
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
        ExpectTrue(pose.bone_count == 4, "rest 位姿骨数应等于骨架骨数");
        ExpectTrue(pose.joint_rotation.size() == 4u, "rest 位姿旋转数组尺寸应为骨数");
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

    // ── C. 蓝骨驱动：BodyRetarget vs 无重定向（数值直搬）──────────────────────
    //
    // 这套用例直接验"两种驱动语义不同"——并**不靠 GL**（纯 CPU 算 glb_pose_）。
    //
    // ⚠️ **构型必须满足两个条件，否则两驱动结果相同、测不出东西**（本用例踩过两次）：
    //   ① 目标的 bind 与源**不同轴**（至少一根骨）；
    //   ② 目标的**父骨也被驱动**（父链上带偏差）。
    //   若不满足②，直搬与重定向会逐位相同（实测差 0.0°）。满足后差别显著（实测 ~83°）。
    //
    // 骨架（4 骨，见 MakeSyntheticSkeleton）：
    //   0 Hips → 1 Spine → { 2 RightHand, 3 LeftHand }
    //   源（红）：无 bind（全恒等）；pose = Spine 绕 Z 30°，Hips 不动。
    //   目标（蓝）：Spine 的 bind 绕 X 90°（异轴）；pose 由被测驱动算。
    //
    // 预期：
    //   · NoRetarget：目标 pose **数值==源 pose**（含 Spine 的 30°）⇒ 两驱动显著不同。
    //   · BodyRetarget：人体随动系不变式成立（偏差在 body 系下一致）。
    static void TestBlueDriveModes() {
        FbxViewerApp app(JPOV::Config{});
        SetupApp(&app);

        // 目标骨架同构，但 **Spine 的 bind 绕自身骨轴（局部 +Y）叠一个不可见 roll**。
        //   ⚠️ 不能拿“绕 X 90°”当“异轴异布局”——那会**真改几何**（Spine 段方向从 +Y 变 +Z），
        //      而 Q_body 只吸收**整体朝向差**，吸收不了“某根骨自己的段方向差”。
        //      本用例要的是“几何一致 + 只有不可见 roll”（BodyRetarget 的目标场景）。
        jpov::SkeletonType& tgt = app.glb_skeleton_for_test();
        tgt = app.skeleton_for_test();
        tgt.bind_rotation.assign(tgt.joints.size(), geom::Quaternion<float>::Identity());
        // Spine 的骨轴 = 它指向子骨的方向 = +Y（rest_offset (0,0.2,0)）⇒ 绕 Y 的 roll 不可见。
        tgt.bind_rotation[1] = RotY(90.0f);
        tgt.Validate();
        app.RebuildBindForTest();

        // 对位必须全命中（同名），否则下面测的是"未命中保持 rest"而非驱动语义。
        ExpectTrue(app.retarget_plan_for_test().matched_bone_count == 4,
                   "异布局测试要求 4 骨全命中（否则测的不是驱动语义）");

        // 源 pose：Spine 绕 Z 30°（父 Hips 不动）。
        jpov::SkeletonPose src_pose =
            jpov::SkeletonPose::Identity(4);
        src_pose.joint_rotation[1] = RotZ(30.0f);

        // (1) NoRetarget：数值直搬 ⇒ 目标 pose **数值==源 pose**。
        app.SetBlueDriveForTest(BlueDrive::kNoRetarget);
        app.SetFramePoseForTest(src_pose);
        app.ComputeGlbPoseForTest();
        ExpectNear(AngleZDeg(app.glb_pose_for_test().joint_rotation[1]), 30.0, 1e-3,
                   "NoRetarget 应把源的 30° 数值原样写入目标 Spine");
        const geom::Quaternion<float> naive_spine =
            app.glb_pose_for_test().joint_rotation[1];

        // (2) BodyRetarget：因目标 Spine 叠了不可见 roll（绕自身轴 90°），
        //     局部数值会与直搬不同（但其实两侧几何一致、结果应更“对”）。
        app.SetBlueDriveForTest(BlueDrive::kBodyRetarget);
        app.ComputeGlbPoseForTest();
        const geom::Quaternion<float> bt_spine =
            app.glb_pose_for_test().joint_rotation[1];
        const float drive_diff = QuatDiffDeg(naive_spine, bt_spine);

        // 核心断言：**几何一致 ⇒ 目标骨指向 == Q_body·源骨指向（零误差）**。
        //   （这正是 BodyRetarget 相对旧逐骨 Q 的关键差异：不可见 roll 不再污染结果。）
        const jpov::SkeletonType& src = app.skeleton_for_test();
        const jpov::SkeletonType& dst = app.glb_skeleton_for_test();
        const geom::Quaternion<float> q_body =
            app.retarget_plan_for_test().q_body;
        const std::vector<geom::Quaternion<float>> ws = WorldRotations(src, src_pose);
        const std::vector<geom::Quaternion<float>> wt =
            WorldRotations(dst, app.glb_pose_for_test());
        for (size_t j = 0; j < ws.size(); ++j) {
            const jpov::Vec3f dS =
                geom::RotateVector(ws[j], jpov::Vec3f(0, 1, 0));
            const jpov::Vec3f want = geom::RotateVector(q_body, dS);
            const jpov::Vec3f dT =
                geom::RotateVector(wt[j], jpov::Vec3f(0, 1, 0));
            float c = want.x() * dT.x() + want.y() * dT.y() + want.z() * dT.z();
            c = std::max(-1.0f, std::min(1.0f, c));
            const float err = std::acos(c) * 180.0f / kPi;
            LOG(INFO) << "  j=" << j << " '" << src.joints[j].name << "' err=" << err
                      << "° want=(" << want.x() << "," << want.y() << "," << want.z()
                      << ") got=(" << dT.x() << "," << dT.y() << "," << dT.z() << ")";
            ExpectTrue(err < 1.0f,
                       "🔴 BodyRetarget 下目标骨指向应 == Q_body·源骨指向");
        }

        // (3) 不变量 1：源 pose 恒等 ⇒ BodyRetarget 目标 pose 恒等（保持自己 rest）。
        app.SetFramePoseForTest(jpov::SkeletonPose::Identity(4));
        app.ComputeGlbPoseForTest();
        for (size_t j = 0; j < app.glb_pose_for_test().joint_rotation.size(); ++j) {
            ExpectTrue(QuatAngleDeg(app.glb_pose_for_test().joint_rotation[j]) < 1e-3f,
                       "🔴 源不动 ⇒ BodyRetarget 下目标也不动（保持自己的 bind 朝向）");
        }
        LOG(INFO) << "OK TestBlueDriveModes（两驱动 Spine 位姿差 " << drive_diff << "°）";
    }
};

}  // namespace jpov_fbx_viewer

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    jpov_fbx_viewer::FbxViewerPlaybackTest::Run();
    LOG(INFO) << "fbx_viewer_playback_test PASSED";
    return 0;
}

// skeleton_retarget.h（人体随动系重定向 BodyRetarget）单元测试
//
// 覆盖（对应 header 文件头“三、不变量”）：
//   · EstimateBodyFrame：M 的定义/三轴/根 yaw/180° 分支点/缺骨退化/ForeArm 回退/官方骨架
//   · BodyRetarget：
//       1. ⭐ **几何一致（任意 bind/rest_offset 自由度 + root 整体旋转）⇒ 零误差**（最硬门禁）
//       2. 有几何朝向真差时：目标骨指向 == Q_body · 源骨指向
//       3. 源 pose 恒等 ⟹ 输出 pose 恒等
//       4. 未命中的目标骨 pose = identity
//       5. 估不出人体随动系 ⇒ FATAL（不静默用错基准）
//       6. Pre-condition：尺寸不符 ⇒ 崩溃

#include "tools/jpov/interface/skeleton_retarget.h"

#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {
namespace {

constexpr float kPi = static_cast<float>(M_PI);

// 造一根骨的便捷函数。
SkeletonJoint MakeJoint(int parent, float x, float y, float z, const std::string& name) {
    SkeletonJoint j;
    j.parent = parent;
    j.rest_offset = Vec3f(x, y, z);
    j.name = name;
    return j;
}

// 绕 Y 轴旋转 deg 度的单位四元数（“整个骨架朝向 / 绕自身骨轴 roll”测试用）。
geom::Quaternion<float> RotYDeg(float deg) {
    return geom::Quaternion<float>::FromAxisAngle(Vec3f(0.0f, 1.0f, 0.0f),
                                                  deg * kPi / 180.0f);
}

// 造一个"双手沿 ±X（左 = +X）"的 T-pose 骨架，root 可带 yaw。
// 结构: 0 Hips(root) → 1 Right<..>(−span) / 2 Left<..>(+span)
SkeletonType MakeTposeHands(const std::string& left_name, const std::string& right_name,
                            float span = 0.5f,
                            const geom::Quaternion<float>& root_bind =
                                geom::Quaternion<float>::Identity()) {
    SkeletonType s;
    s.joints.push_back(MakeJoint(kSkeletonNoParent, 0.0f, 1.0f, 0.0f, "mixamorig:Hips"));
    s.joints.push_back(MakeJoint(0, -span, 0.0f, 0.0f, right_name));  // 右 = −X
    s.joints.push_back(MakeJoint(0, +span, 0.0f, 0.0f, left_name));   // 左 = +X
    s.bind_rotation.assign(3, geom::Quaternion<float>::Identity());
    s.bind_rotation[0] = root_bind;
    s.Validate();
    return s;
}

// M 的定义：M 的**三列** = 人体随动系三轴在 world 下的表达 ⇒
//   RotateVector(M, 单位轴) 应 == 对应 axis_*。这条把"定义"钉在代码上。
void ExpectColumnsAreAxes(const BodyFrame& f) {
    EXPECT_NEAR((geom::RotateVector(f.rotation, Vec3f(1, 0, 0)) - f.axis_left).Norm(),
                0.0f, 1e-4f) << "M 第 0 列应为 X_body(左)";
    EXPECT_NEAR((geom::RotateVector(f.rotation, Vec3f(0, 1, 0)) - f.axis_up).Norm(),
                0.0f, 1e-4f) << "M 第 1 列应为 Y_body(上)";
    EXPECT_NEAR((geom::RotateVector(f.rotation, Vec3f(0, 0, 1)) - f.axis_forward).Norm(),
                0.0f, 1e-4f) << "M 第 2 列应为 Z_body(前)";
}

// 沿树算**全部**骨的世界朝向（一般递归，含父骨 pose 的影响）。
//   W(j) = W(parent) ⊗ B(j) ⊗ P(j)
// ⚠️ **不能**用 `Rb(j) ⊗ P(j)` 代入：那个等式只在**父骨处于 rest**时成立，
//    父骨一有 pose，W(parent) 就把父骨的偏差带进来。本文件曾因此写出错误断言（伪失败）。
std::vector<geom::Quaternion<float>> WorldRotations(const SkeletonType& skel,
                                                    const SkeletonPose& pose) {
    const size_t n = skel.joints.size();
    std::vector<geom::Quaternion<float>> w(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> b = skel.bind_rotation.empty()
                                             ? geom::Quaternion<float>::Identity()
                                             : skel.bind_rotation[j];
        const geom::Quaternion<float> p = (pose.joint_rotation.empty())
                                             ? geom::Quaternion<float>::Identity()
                                             : pose.joint_rotation[j];
        const geom::Quaternion<float> local = (b * p).Normalized();
        const int par = skel.joints[j].parent;
        w[j] = (par == kSkeletonNoParent)
                   ? local
                   : (w[static_cast<size_t>(par)] * local).Normalized();
    }
    return w;
}

// 沿树复算某骨的**世界朝向**（独立实现，用于偏差断言）。
geom::Quaternion<float> WorldRotationOf(const SkeletonType& skel, const SkeletonPose& pose,
                                        int idx) {
    const std::vector<geom::Quaternion<float>> all = WorldRotations(skel, pose);
    return all[static_cast<size_t>(idx)];
}

// ★ 偏差（deviation）：该骨相对**自己 rest** 转了多少 —— 本文件的核心不变量。
//   deviation(j) = Rb(j)⁻¹ ⊗ W(j)
//   ⚠️ 这就是 header 宣称被保持的那个量，且**必须按此定义比**（不能换成"父系里的 P"）。
//   验证过的事实（dbg 实测）：
//     · deviation 逐骨**精确**一致（差值 0）；
//     · 而"局部 pose P"**不**逐骨一致（子骨会因父骨转动而不同）——P 只有根一级才一致。
//   所以断言必须落在 deviation 上；拿 P 去比会得到"看上去有 bug"的伪失败（本文件曾踩）。
struct Deviation {
    geom::Quaternion<float> q;      // 旋转量（在本骨 rest 世界里表达）
    float angle_deg = 0.0f;         // q 的旋转角（∈[0,180]）
};

// 按 header 定义复算偏差（独立路径：自己沿树算 W，不调被测的 RetargetPose）。
Deviation ComputeDeviation(const SkeletonType& skel, const SkeletonPose& pose, int idx) {
    const std::vector<geom::Quaternion<float>> rb = RestWorldRotations(skel);
    const size_t i = static_cast<size_t>(idx);
    const geom::Quaternion<float> world = WorldRotationOf(skel, pose, idx);
    Deviation d;
    geom::Quaternion<float> q = (rb[i].Conjugate() * world).Normalized();
    // 规范化到 w ≥ 0（q 与 −q 表示同一旋转，不规范化会让比对结果随机翻符号）。
    if (q.w < 0.0f) {
        q = geom::Quaternion<float>(-q.x, -q.y, -q.z, -q.w);
    }
    d.q = q;
    d.angle_deg = QuatAngleDeg(q, geom::Quaternion<float>::Identity());
    return d;
}

// ════════════════════════════════════════════════════════════════════════════
//  EstimateBodyFrame：M 的定义与三轴
// ════════════════════════════════════════════════════════════════════════════

// 同布局：两侧 bind 朝向逐骨相同（只差骨长）⟹ Q 应逐骨 ≈ 恒等。
TEST(SkeletonRetargetTest, BodyFrameTposeFacingZIsIdentity) {
    const SkeletonType s = MakeTposeHands("mixamorig:LeftHand", "mixamorig:RightHand");
    const BodyFrame f = EstimateBodyFrame(s);
    ASSERT_TRUE(f.valid);
    EXPECT_NEAR(f.yaw_deg, 0.0f, 1e-3f);
    EXPECT_NEAR(QuatAngleDeg(f.rotation, geom::Quaternion<float>::Identity()), 0.0f, 1e-3f);
    EXPECT_NEAR((f.axis_left - Vec3f(1, 0, 0)).Norm(), 0.0f, 1e-4f);
    EXPECT_NEAR((f.axis_up - Vec3f(0, 1, 0)).Norm(), 0.0f, 1e-4f);
    EXPECT_NEAR((f.axis_forward - Vec3f(0, 0, 1)).Norm(), 0.0f, 1e-4f);
    EXPECT_EQ(f.left_bone, "mixamorig:LeftHand");
    EXPECT_EQ(f.right_bone, "mixamorig:RightHand");
    ExpectColumnsAreAxes(f);
}

// 整具骨架绕 Y 转 θ ⇒ M 应 = 同角度 yaw（含 ±90°、180°、负角）。
TEST(SkeletonRetargetTest, BodyFrameFollowsRootYaw) {
    for (const float deg : {30.0f, 90.0f, 180.0f, -90.0f, -150.0f}) {
        const geom::Quaternion<float> q = RotYDeg(deg);
        const SkeletonType s =
            MakeTposeHands("mixamorig:LeftHand", "mixamorig:RightHand", 0.5f, q);
        const BodyFrame f = EstimateBodyFrame(s);
        ASSERT_TRUE(f.valid) << "deg=" << deg;
        EXPECT_NEAR(QuatAngleDeg(f.rotation, q), 0.0f, 1e-2f) << "deg=" << deg;
        ExpectColumnsAreAxes(f);
        // Z = X × Y 必须成立（右手系）
        const Vec3f cross(f.axis_left.y() * f.axis_up.z() - f.axis_left.z() * f.axis_up.y(),
                          f.axis_left.z() * f.axis_up.x() - f.axis_left.x() * f.axis_up.z(),
                          f.axis_left.x() * f.axis_up.y() - f.axis_left.y() * f.axis_up.x());
        EXPECT_NEAR((cross - f.axis_forward).Norm(), 0.0f, 1e-4f) << "deg=" << deg;
    }
}

// 180° 是轴角/四元数表示的**分支点**：这里必须稳（不能劈开/翻错）。
TEST(SkeletonRetargetTest, BodyFrameHalfTurnIsStable) {
    const SkeletonType s =
        MakeTposeHands("mixamorig:LeftHand", "mixamorig:RightHand", 0.5f, RotYDeg(180.0f));
    const BodyFrame f = EstimateBodyFrame(s);
    ASSERT_TRUE(f.valid);
    EXPECT_NEAR((f.axis_left - Vec3f(-1, 0, 0)).Norm(), 0.0f, 1e-4f);
    EXPECT_NEAR((f.axis_forward - Vec3f(0, 0, -1)).Norm(), 0.0f, 1e-4f);
    EXPECT_NEAR(std::fabs(f.yaw_deg), 180.0f, 1e-2f);
    ExpectColumnsAreAxes(f);
}

// 手腕缺 → 退到紧邻关节（前臂），并报出实际用的骨名。
TEST(SkeletonRetargetTest, BodyFrameFallsBackToForeArm) {
    const SkeletonType s =
        MakeTposeHands("mixamorig:LeftForeArm", "mixamorig:RightForeArm");
    const BodyFrame f = EstimateBodyFrame(s);
    ASSERT_TRUE(f.valid);
    EXPECT_EQ(f.left_bone, "mixamorig:LeftForeArm");
    EXPECT_EQ(f.right_bone, "mixamorig:RightForeArm");
    EXPECT_NEAR(f.yaw_deg, 0.0f, 1e-3f);
}

// 两档骨名都缺 ⇒ invalid（不猜、不 fallback）。
TEST(SkeletonRetargetTest, BodyFrameInvalidWhenBonesMissing) {
    const SkeletonType s = MakeTposeHands("mixamorig:LeftWristX", "mixamorig:RightWristX");
    EXPECT_FALSE(EstimateBodyFrame(s).valid);
}

// 连线退化为沿 up（去 Y 后近零）⇒ invalid（不是"凑一个"）。
TEST(SkeletonRetargetTest, BodyFrameInvalidWhenSpanParallelToUp) {
    SkeletonType s;
    s.joints.push_back(MakeJoint(kSkeletonNoParent, 0.0f, 0.0f, 0.0f, "mixamorig:Hips"));
    s.joints.push_back(MakeJoint(0, 0.0f, 0.3f, 0.0f, "mixamorig:RightHand"));
    s.joints.push_back(MakeJoint(0, 0.0f, 0.6f, 0.0f, "mixamorig:LeftHand"));
    s.bind_rotation.assign(3, geom::Quaternion<float>::Identity());
    s.Validate();
    EXPECT_FALSE(EstimateBodyFrame(s).valid);
}

// 骨名按**后缀**匹配：不带 mixamorig: 前缀也能用。
TEST(SkeletonRetargetTest, BodyFrameMatchesBySuffixRegardlessOfPrefix) {
    const SkeletonType s = MakeTposeHands("LeftHand", "RightHand");
    const BodyFrame f = EstimateBodyFrame(s);
    ASSERT_TRUE(f.valid);
    EXPECT_NEAR(f.yaw_deg, 0.0f, 1e-3f);
}

// 真实规模：官方 Mixamo23 骨架（源自 Hip Hop Dancing.fbx）应面朝 +Z、左 = +X。
TEST(SkeletonRetargetTest, BodyFrameOfOfficialMixamo23) {
    const SkeletonType s = Mixamo23Skeleton(1.60f);
    const BodyFrame f = EstimateBodyFrame(s);
    ASSERT_TRUE(f.valid);
    EXPECT_EQ(f.left_bone, "mixamorig:LeftHand");
    EXPECT_EQ(f.right_bone, "mixamorig:RightHand");
    EXPECT_NEAR((f.axis_up - Vec3f(0, 1, 0)).Norm(), 0.0f, 1e-6f);
    // 官方 T-pose 手水平张开 ⇒ 左右轴应几乎正好是 +X（左手在 +X）⇒ yaw ≈ 0（面朝 +Z）
    EXPECT_NEAR(f.axis_left.x(), 1.0f, 0.05f) << "轴 = ("
        << f.axis_left.x() << "," << f.axis_left.y() << "," << f.axis_left.z() << ")";
    EXPECT_NEAR(f.yaw_deg, 0.0f, 3.0f) << "官方 Mixamo 骨架应面朝 +Z（左 = +X）";
    LOG(INFO) << "Mixamo23 人体随动系: yaw=" << f.yaw_deg << "° axis_left=("
              << f.axis_left.x() << "," << f.axis_left.y() << "," << f.axis_left.z()
              << ") axis_forward=(" << f.axis_forward.x() << "," << f.axis_forward.y()
              << "," << f.axis_forward.z() << ")";
    ExpectColumnsAreAxes(f);
}

// ════════════════════════════════════════════════════════════════════════════
//  12. BodyRetarget（人体随动系版）—— @Danis 点名的核心不变量
// ════════════════════════════════════════════════════════════════════════════

// 造一套“几何与源一致”的骨架：给定可任意摆弄的 bind/rest_offset，但**骨指向**要向
// 源看齐（再整体叠一个 root yaw）。做法：先造几何标准的 T-pose，再逐骨乘一个任意
// 的“绕自身骨轴 roll”——它对骨指向/位置（几乎）无影响，但会让 bind 值大不相同。
//
// 结构：0 Hips(根) → 1 Spine → 2 LShoulder → 3 LArm → 4 LForeArm → 5 LHand
//                └→ 6 RShoulder → ... （简化为一条右侧臂链，足以覆盖多级父链）
struct GeoChain {
    SkeletonType type;
};

// 造一条人形臂链：**先在 world 里摆好几何**（面朝 +Z、左 = +X、手臂水平张开），
// 再由几何反推每骨的 `rest_offset`（相对父的平移）与 `bind_rotation`（相对父的旋转，
// 可能再叠一个任意“绕自身骨轴”的不可见 roll），并整体叠一个 root yaw。
//
//   这样做保证了：① `rest_offset` 真在**父系**里（之前版本误把 world 偏移当父系偏移）；
//   ② “叠 roll” 真的是绕自身骨轴（对骨指向/位置无影响）。
//
// 几何（world）：胯(0,1,0) → 脊(0,1.2,0) →{ 左肩(0.18,1.2,0) → 左臂(0.36,1.2,0)
//                                                 → 左前臂(0.54,1.2,0) → 左手(0.72,1.2,0)
//                                              右肩(-0.18,1.2,0) → ... 右腕(-0.72,1.2,0) }
// 每骨的“骨指向”= 它指向**子骨**的方向（叶子骨沿用父的段方向）。
GeoChain MakeGeoArmChain(float root_yaw_deg = 0.0f,
                         const std::vector<float>& rolls = {}) {
    const char* names[] = {"mixamorig:Hips", "mixamorig:Spine",
                           "mixamorig:LeftShoulder", "mixamorig:LeftArm",
                           "mixamorig:LeftForeArm", "mixamorig:LeftHand",
                           "mixamorig:RightShoulder", "mixamorig:RightArm",
                           "mixamorig:RightForeArm", "mixamorig:RightHand"};
    const int n = 10;
    // world 下的关节位置（面朝 +Z）与父索引。
    struct Spec { int parent; float x, y, z; };
    const Spec specs[] = {
        {kSkeletonNoParent, 0.00f, 1.00f, 0.0f},  // 0 Hips
        {0,                 0.00f, 1.20f, 0.0f},  // 1 Spine
        {1,                 0.18f, 1.20f, 0.0f},  // 2 LShoulder
        {2,                 0.36f, 1.20f, 0.0f},  // 3 LArm
        {3,                 0.54f, 1.20f, 0.0f},  // 4 LForeArm
        {4,                 0.72f, 1.20f, 0.0f},  // 5 LHand
        {1,                -0.18f, 1.20f, 0.0f},  // 6 RShoulder
        {6,                -0.36f, 1.20f, 0.0f},  // 7 RArm
        {7,                -0.54f, 1.20f, 0.0f},  // 8 RForeArm
        {8,                -0.72f, 1.20f, 0.0f},  // 9 RHand
    };
    // 每骨的“指向子骨”方向（叶子骨用“父→自己”的段方向）——即几何上的骨轴。
    Vec3f dir[n];
    for (int i = 0; i < n; ++i) {
        // 找 i 的第一个子；没子就用 父→i 的段方向。
        int child = -1;
        for (int k = 0; k < n; ++k) {
            if (specs[k].parent == i) { child = k; break; }
        }
        const int ref = (child >= 0) ? child : i;
        const int base = (child >= 0) ? i : specs[i].parent;
        dir[i] = Vec3f(specs[ref].x - specs[base].x, specs[ref].y - specs[base].y,
                       specs[ref].z - specs[base].z).Unit();
    }
    // 造旋转：让该骨局部 +Y 指向 dir[i]。逐骨求解“使 R·ŷ = dir”的最小旋转。
    auto rot_from_y = [](const Vec3f& d) {
        const Vec3f from(0.0f, 1.0f, 0.0f);
        const float c = std::max(-1.0f, std::min(1.0f, from.y() * d.y()));
        // 通用（from 恒为 +Y，可简化）：轴 = ŷ × d，角 = acos(d.y())。
        const Vec3f ax(1.0f * d.z() - 0.0f * d.y(), 0.0f * d.x() - 0.0f * d.z(),
                       0.0f * d.y() - 1.0f * d.x());
        const float s = ax.Norm();
        if (s < 1e-9f) {
            if (c > 0.0f) return geom::Quaternion<float>::Identity();
            return geom::Quaternion<float>(1.0f, 0.0f, 0.0f, 0.0f);  // 180° 绕 X
        }
        const geom::Quaternion<float> q(ax.x() / s, ax.y() / s, ax.z() / s,
                                        std::cos(std::acos(c) * 0.5f));
        const float half = std::acos(c) * 0.5f;
        return geom::Quaternion<float>(ax.x() / s * std::sin(half),
                                       ax.y() / s * std::sin(half),
                                       ax.z() / s * std::sin(half), std::cos(half))
            .Normalized();
    };

    // 整体 yaw 作用的 world 位置与方向。
    const geom::Quaternion<float> qyaw = RotYDeg(root_yaw_deg);
    Vec3f pos[n];
    for (int i = 0; i < n; ++i) {
        pos[i] = geom::RotateVector(qyaw, Vec3f(specs[i].x, specs[i].y, specs[i].z));
    }
    Vec3f wdir[n];
    for (int i = 0; i < n; ++i) {
        wdir[i] = geom::RotateVector(qyaw, dir[i]);
    }

    GeoChain g;
    g.type.joints.resize(n);
    g.type.bind_rotation.assign(n, geom::Quaternion<float>::Identity());
    // 逐层算：rest 世界旋转 Rw(i) = Rw(父) · bind(i)；要求 Rw(i)·ŷ = wdir[i]。
    // ⇒ bind(i) = Rw(父)⁻¹ · (使 ŷ → wdir[i] 的旋转)，再叠不可见 roll。
    std::vector<geom::Quaternion<float>> rw(n, geom::Quaternion<float>::Identity());
    for (int i = 0; i < n; ++i) {
        const int p = specs[i].parent;
        const geom::Quaternion<float> rw_parent =
            (p == kSkeletonNoParent) ? geom::Quaternion<float>::Identity() : rw[p];
        // 本骨局部系里“指向子骨”的方向 = Rw(父)⁻¹·wdir[i]（因为 bind 后要对齐 wdir）。
        const Vec3f d_local = geom::RotateVector(rw_parent.Conjugate(), wdir[i]);
        geom::Quaternion<float> bind = rot_from_y(d_local);
        // 叠不可见 roll（绕自身骨轴 = 局部 +Y —— 不影响 Rw·ŷ）。
        float roll_deg = 0.0f;
        if (i < static_cast<int>(rolls.size())) roll_deg = rolls[static_cast<size_t>(i)];
        bind = (bind * RotYDeg(roll_deg)).Normalized();
        g.type.bind_rotation[i] = bind;
        rw[i] = (rw_parent * bind).Normalized();
        // rest_offset：父系里的位移 = Rw(父)⁻¹ · (pos[i] − pos[父])。
        if (p == kSkeletonNoParent) {
            g.type.joints[i].rest_offset = pos[i];  // 根：在骨架空间
        } else {
            const Vec3f delta(pos[i].x() - pos[p].x(), pos[i].y() - pos[p].y(),
                              pos[i].z() - pos[p].z());
            g.type.joints[i].rest_offset = geom::RotateVector(rw_parent.Conjugate(), delta);
        }
        g.type.joints[i].parent = p;
        g.type.joints[i].name = names[i];
    }
    g.type.Validate();
    return g;
}

// 把骨架绕 Y 转 yaw（几何整体旋转）：改 root 的 bind + 把所有 rest_offset 跟着转。
void YawSkeletonInPlace(SkeletonType* s, float yaw_deg) {
    const geom::Quaternion<float> q = RotYDeg(yaw_deg);
    for (auto& j : s->joints) {
        j.rest_offset = geom::RotateVector(q, j.rest_offset);
    }
    s->bind_rotation[0] = (q * s->bind_rotation[0]).Normalized();
    s->Validate();
}

// ⭐ 核心不变量（Danis 定）：几何一致的两个骨人（不管 bind/rest_offset 自由度怎么变）
//    ⇒ 重定向 **零误差**；再加一个 root 整体旋转也仍然零误差。
TEST(SkeletonRetargetTest, BodyRetargetZeroErrorForIdenticalGeometry) {
    // 源：几何标准、无不可见 roll。
    const GeoChain src = MakeGeoArmChain();

    // 目标：**几何一致**，但故意叠一堆不可见的 roll（包括 180° 这个分支点）。
    //   再叠一个 root 整体 yaw（= 资产朝向不同）。
    for (const float yaw : {0.0f, 37.0f, 90.0f, 180.0f}) {
        GeoChain tgt = MakeGeoArmChain(yaw,
                                       {0.0f, 0.0f, 180.0f, 175.0f, -120.0f, 90.0f,
                                        0.0f, 180.0f, 70.0f, -15.0f});
        const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt.type, src.type);
        EXPECT_EQ(plan.matched_bone_count, 10) << "yaw=" << yaw;

        // 源给一段动作（每骨绕世界某轴转）。
        SkeletonPose sp = SkeletonPose::Identity(10);
        sp.joint_rotation[1] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), 0.3f);
        sp.joint_rotation[3] = geom::Quaternion<float>::FromAxisAngle(Vec3f(1, 0, 0), 0.4f);
        sp.joint_rotation[4] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), -0.5f);

        const SkeletonPose out = BodyRetargetOnePose(tgt.type, src.type, sp);

        // 期望：目标的**世界骨指向** = Q_body 作用于源的世界骨指向。
        //   （几何一致 + 目标自己带 yaw ⇒ 只差 Q_body 这一个整体旋转；误差必须为 0。）
        //   ⚠️ 不能直接比“源指向 vs 目标指向”：目标带 yaw 时两者本来就差那个 yaw；
        //      要先把源的指向用 Q_body 搬过去再比（Q_body 就是两侧人体系的整体差）。
        const std::vector<Quatf> ws = WorldRotations(src.type, sp);
        const std::vector<Quatf> wt = WorldRotations(tgt.type, out);
        for (size_t j = 0; j < ws.size(); ++j) {
            const Vec3f dS = geom::RotateVector(ws[j], Vec3f(0, 1, 0));
            const Vec3f want = geom::RotateVector(plan.q_body, dS);
            const Vec3f dT = geom::RotateVector(wt[j], Vec3f(0, 1, 0));
            float c = want.x() * dT.x() + want.y() * dT.y() + want.z() * dT.z();
            c = std::max(-1.0f, std::min(1.0f, c));
            EXPECT_NEAR(std::acos(c) * 180.0f / kPi, 0.0f, 0.5f)
                << "yaw=" << yaw << " joint=" << j << " '"
                << src.type.joints[j].name << "' 骨指向应 = Q_body·源指向（零误差）";
        }
        LOG(INFO) << "BodyRetarget 几何一致 yaw=" << yaw << "°: Q_body="
                  << plan.q_body_angle_deg << "° 全部骨指向零误差";
    }
}

// 跨朝向能力：源面朝 +Z、目标面朝 +X（几何真不同，非 roll）⇒ 动作仍应正确对应。
TEST(SkeletonRetargetTest, BodyRetargetHandlesRealOrientationDifference) {
    const GeoChain src = MakeGeoArmChain(/*yaw=*/0.0f);
    GeoChain tgt = MakeGeoArmChain(/*yaw=*/90.0f);  // 真差 90°
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt.type, src.type);
    // Q_body 应 ≈ 90°（就是那两侧整体朝向差）。
    EXPECT_NEAR(plan.q_body_angle_deg, 90.0f, 1.0f);

    SkeletonPose sp = SkeletonPose::Identity(10);
    sp.joint_rotation[3] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), 0.5f);
    const SkeletonPose out = BodyRetargetOnePose(tgt.type, src.type, sp);

    // 目标骨指向 应 = Q_body 作用于源骨指向（几何一致的旋转对应）。
    const std::vector<Quatf> ws = WorldRotations(src.type, sp);
    const std::vector<Quatf> wt = WorldRotations(tgt.type, out);
    for (size_t j = 0; j < ws.size(); ++j) {
        const Vec3f dS = geom::RotateVector(ws[j], Vec3f(0, 1, 0));
        const Vec3f want = geom::RotateVector(plan.q_body, dS);
        const Vec3f dT = geom::RotateVector(wt[j], Vec3f(0, 1, 0));
        float c = want.x() * dT.x() + want.y() * dT.y() + want.z() * dT.z();
        c = std::max(-1.0f, std::min(1.0f, c));
        EXPECT_NEAR(std::acos(c) * 180.0f / kPi, 0.0f, 0.5f)
            << "joint=" << j << " 应 = Q_body·源指向";
    }
}

// 不变量 1：源 pose 恒等 ⇒ 目标 pose 恒等（保持自己的 rest）。
TEST(SkeletonRetargetTest, BodyRetargetIdentitySourceGivesIdentityTarget) {
    const GeoChain src = MakeGeoArmChain();
    const GeoChain tgt = MakeGeoArmChain(45.0f, {0, 0, 180.0f, 0, 0, 0, 0, 0, 0, 0});
    const SkeletonPose out =
        BodyRetargetOnePose(tgt.type, src.type, SkeletonPose::Identity(10));
    for (size_t j = 0; j < out.joint_rotation.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[j], geom::Quaternion<float>::Identity()),
                    0.0f, 1e-3f)
            << "joint " << j << " 源不动 ⇒ 目标保持自己 rest";
    }
}

// 未命中的目标骨保持 rest；命中统计正确。
TEST(SkeletonRetargetTest, BodyRetargetUnmatchedStaysAtRest) {
    SkeletonType src = MakeGeoArmChain().type;
    SkeletonType tgt = MakeGeoArmChain(0.0f, {0, 0, 180.0f, 0, 0, 0, 0, 0, 0, 0}).type;
    // 把目标两根**不影响人体随动系**的骨改名（=> 未命中）。
    //   人体随动系只看 Left/Right Hand（优先）或 ForeArm ⇒ 改臂/肩不影响它。
    tgt.joints[3].name = "mixamorig:UnknownArm";
    tgt.joints[4].name = "mixamorig:UnknownForeArm";
    tgt.Validate();
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt, src);
    EXPECT_TRUE(plan.target_frame.valid) << "改名不影响人体随动系（靠 Hand）";
    EXPECT_EQ(plan.matched_bone_count, 8);
    EXPECT_EQ(plan.unmapped_target_bones.size(), 2u);
    EXPECT_EQ(plan.source_of_target[3], -1);

    SkeletonPose sp = SkeletonPose::Identity(10);
    sp.joint_rotation[4] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.7f);
    const SkeletonPose out = BodyRetargetOnePose(tgt, src, sp);
    EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[3], geom::Quaternion<float>::Identity()),
                0.0f, 1e-4f)
        << "未命中的目标骨必须保持自身 rest";
    EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[4], geom::Quaternion<float>::Identity()),
                0.0f, 1e-4f)
        << "未命中的目标骨必须保持自身 rest";
}

// 两套骨架几何一致且都没有 roll 时，BodyRetarget 应与旧版（逐骨 Q）结果一致。
// （两者只在 roll 上分峔；无 roll ⇒ 应该一样——这是“新版是旧版的推广”的自证。）
// 估不出人体随动系 ⇒ FATAL（不静默用错基准）。
TEST(SkeletonRetargetTest, BodyRetargetPlanDiesWithoutBodyFrame) {
    const SkeletonType src = MakeGeoArmChain().type;
    SkeletonType tgt = MakeGeoArmChain().type;
    for (auto& j : tgt.joints) {
        j.name = "" ;  // 没有任何可对位骨名 ⇒ 也估不出人体随动系
    }
    EXPECT_DEATH(BuildBodyRetargetPlan(tgt, src), "人体随动系");
}

TEST(SkeletonRetargetTest, WrongSourcePoseSizeDies) {
    // 用真实规模的骨架（4 骨人形）建 plan，再喂尺寸不符的源 pose。
    const SkeletonType src = MakeGeoArmChain().type;
    const SkeletonType tgt = MakeGeoArmChain().type;
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt, src);

    SkeletonPose bad;
    bad.bone_count = 1;
    bad.joint_rotation.resize(1);  // 与源骨数（10）不符 → 应崩溃

    SkeletonPose out;
    EXPECT_DEATH(BodyRetargetPose(plan, bad, &out), "source_pose 尺寸");
}

// ══════════════════════════════════════════════════════════════════════════
//  root_offset（root-motion）搬运 —— 2026-09-20 接线
//  定稿定义：根骨在【其父坐标系】下、相对【其 bind 位置】的平移量。
//  重定向式：root_offset_t = Q_body · root_offset_s · (leg_t / leg_s)
// ══════════════════════════════════════════════════════════════════════════

// 缩放一条 GeoChain 的根高度（= 腰高）：只改根的 rest_offset.y（及其子树位置一起搬）。
// 这样两套骨架"几何一致、只差身高" —— 正是腿长比要覆盖的场景。
SkeletonType ScaledByRootHeight(SkeletonType s, float new_root_y) {
    const float old = s.joints[0].rest_offset.y();
    const float k = new_root_y / old;
    for (auto& j : s.joints) {
        auto sc = [k](float v) { return v * k; };
        j.rest_offset = Vec3f(sc(j.rest_offset.x()), sc(j.rest_offset.y()), sc(j.rest_offset.z()));
    }
    s.Validate();
    return s;
}

// ① 向后兼容：源 root_offset = 0 ⟹ 目标 root_offset = 0。
//    （这是"既有 gold 零回归"的根据 —— 接线前输出恒 0。）
TEST(SkeletonRetargetTest, RootOffsetZeroInZeroOut) {
    const SkeletonType src = MakeGeoArmChain().type;
    const SkeletonType tgt = MakeGeoArmChain().type;
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt, src);

    SkeletonPose sp = SkeletonPose::Identity(10);
    sp.root_offset = Vec3f(0.0f, 0.0f, 0.0f);  // 纯原地动作
    SkeletonPose out;
    BodyRetargetPose(plan, sp, &out);
    EXPECT_NEAR(out.root_offset.x(), 0.0f, 1e-6f);
    EXPECT_NEAR(out.root_offset.y(), 0.0f, 1e-6f);
    EXPECT_NEAR(out.root_offset.z(), 0.0f, 1e-6f);
}

// ② 只差身高（腿长）的两套骨架：位移按 leg_t/leg_s 等比缩放，方向不变（Q_body 恒等）。
TEST(SkeletonRetargetTest, RootOffsetScaledByLegLengthRatio) {
    const SkeletonType src = MakeGeoArmChain().type;                 // 根高 1.00
    const SkeletonType tgt = ScaledByRootHeight(src, 2.00f);         // 根高 2.00 ⇒ ratio = 2
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt, src);

    EXPECT_NEAR(plan.source_leg_length, 1.00f, 1e-4f);
    EXPECT_NEAR(plan.target_leg_length, 2.00f, 1e-4f);
    EXPECT_NEAR(plan.root_offset_scale, 2.00f, 1e-4f);
    // 两侧几何只差尺度（朝向一致）⇒ Q_body 应为恒等。
    EXPECT_NEAR(plan.q_body_angle_deg, 0.0f, 1e-3f)
        << "只差身高时两侧人体随动系应同向";

    SkeletonPose sp = SkeletonPose::Identity(10);
    sp.root_offset = Vec3f(0.10f, 0.00f, 0.30f);  // 源走了 (0.1, 0, 0.3)
    SkeletonPose out;
    BodyRetargetPose(plan, sp, &out);
    // 期望 = 源 × 2（腿长比），方向不变。
    EXPECT_NEAR(out.root_offset.x(), 0.20f, 1e-4f);
    EXPECT_NEAR(out.root_offset.y(), 0.00f, 1e-4f);
    EXPECT_NEAR(out.root_offset.z(), 0.60f, 1e-4f);
}

// ③ 朝向差 90°：位移随之旋转（验证 Q_body 生效）—— 源"往前走"应变成目标系的对应方向，
//    而不是照搬成源系的轴。
TEST(SkeletonRetargetTest, RootOffsetRotatedByQBody) {
    const SkeletonType src = MakeGeoArmChain(0.0f).type;    // 面朝 +Z
    const SkeletonType tgt = MakeGeoArmChain(90.0f).type;   // 整体 yaw 90°
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(tgt, src);
    EXPECT_NEAR(plan.q_body_angle_deg, 90.0f, 1e-2f) << "Q_body 应捕捉 90° 朝向差";

    SkeletonPose sp = SkeletonPose::Identity(10);
    sp.root_offset = Vec3f(0.0f, 0.0f, 1.0f);  // 源朝自己的"前"走 1 单位
    SkeletonPose out;
    BodyRetargetPose(plan, sp, &out);

    // 模长守恒（纯旋转 + 等比缩放；此处 ratio ≈ 1，因为两骨架同高）。
    EXPECT_NEAR(plan.root_offset_scale, 1.0f, 1e-3f);
    EXPECT_NEAR(out.root_offset.Norm(), 1.0f, 1e-4f) << "位移模长应守恒";
    // 方向被 Q_body 转过 ⇒ 与原向量不同（除非 Q_body 恒等，而此处是 90°）。
    const float dot = out.root_offset.x() * sp.root_offset.x() +
                      out.root_offset.y() * sp.root_offset.y() +
                      out.root_offset.z() * sp.root_offset.z();
    EXPECT_NEAR(dot, 0.0f, 1e-3f) << "90° 朝向差下位移应正交于原方向（已被旋转）";
}

// ④ 退化骨架（根高度 ≤ 0）建 plan 时应 FATAL，不静默给一个错的比值。
TEST(SkeletonRetargetTest, RootOffsetScaleDiesOnDegenerateRootHeight) {
    // 把 Hips 高度抬到 y=0（其余骨几何保留，以便人体随动系仍能估出）——
    // 这时骨盆高度退化，腿长比无意义 ⇒ 应 FATAL，不猜一个错的比值。
    const SkeletonType src = MakeGeoArmChain().type;
    SkeletonType tgt = MakeGeoArmChain().type;
    // Hips（idx 0）高度置 0，子树不动（几何仍足够估人体随动系：靠左右 Hand 的 X 向连线）。
    tgt.joints[0].rest_offset = Vec3f(tgt.joints[0].rest_offset.x(), 0.0f,
                                      tgt.joints[0].rest_offset.z());
    tgt.Validate();
    // 前置断言：确实退化，但人体随动系仍可估（否则测的不是腿长退化）。
    ASSERT_LE(RestWorldPositions(tgt)[0].y(), 0.0f);
    ASSERT_TRUE(EstimateBodyFrame(tgt).valid) << "人体随动系应仍可估（否则测的不是腿长退化）";
    EXPECT_DEATH(BuildBodyRetargetPlan(tgt, src), "骨盆高度");
}

// ⑤ 量"腿长"用【Hips 骨名】而非 joints[0] —— 针对 Tripo glb 的真实拓扑：
//    joints[0] 是 'Root' 包装层（高度 ≈ 0），真正的 Hips 是它的子骨。
//    若错用 joints[0].y，比值会变成 0/腰高 或 腰高/0 ⇒ 崩或错。本用例锁住正确行为。
TEST(SkeletonRetargetTest, RootOffsetScaleUsesHipsNotWrapperRoot) {
    const SkeletonType src = MakeGeoArmChain().type;  // joints[0] 就是 Hips（y=1.00）

    // 仿造 glb 拓扑：前面插一个高度≈0 的包装 Root，把原 Hips 挂到它下面。
    SkeletonType tgt = MakeGeoArmChain().type;
    SkeletonType wrapped;
    wrapped.joints.resize(tgt.joints.size() + 1);
    wrapped.bind_rotation.resize(tgt.joints.size() + 1);
    wrapped.joints[0].parent = kSkeletonNoParent;
    wrapped.joints[0].name = "Root";                              // 包装层
    wrapped.joints[0].rest_offset = Vec3f(0.0f, 0.0f, -0.0045f);  // 导出残差（高度≈0）
    wrapped.bind_rotation[0] = geom::Quaternion<float>::Identity();
    for (size_t i = 0; i < tgt.joints.size(); ++i) {
        SkeletonJoint j = tgt.joints[i];
        j.parent = (j.parent == kSkeletonNoParent) ? 0 : j.parent + 1;
        wrapped.joints[i + 1] = j;
        wrapped.bind_rotation[i + 1] = tgt.bind_rotation[i];
    }
    wrapped.Validate();
    ASSERT_LE(RestWorldPositions(wrapped)[0].y(), 0.01f) << "包装层高度应≈0";

    const BodyRetargetPlan plan = BuildBodyRetargetPlan(wrapped, src);
    // 关键：腿长取的是被包装的 Hips 高度（1.00），不是包装层的 0。
    EXPECT_NEAR(plan.target_leg_length, 1.00f, 1e-3f)
        << "必须按骨名找 Hips，不能用 joints[0] 包装层（其高度≈0）";
    EXPECT_NEAR(plan.root_offset_scale, 1.00f, 1e-3f);
}

}  // namespace
}  // namespace jpov

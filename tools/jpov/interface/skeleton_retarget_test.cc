// skeleton_retarget.h（Q 桥接量重定向）单元测试
//
// 覆盖（对应 header 文件头"三、不变量"）：
//   1. Q 的定义与取值：Q(j) = Rb_T(j) ⊗ Rb_S(s(j))⁻¹；同布局骨架 ⟹ Q ≈ 恒等（最硬门禁）
//   2. 骨名对齐：命中数、未命中表、空名不参与
//   3. 不变量 1：源 pose 恒等 ⟹ 输出 pose 恒等
//   4. 不变量 2：命中骨 deviation_T == deviation_S（世界系偏差逐骨一致）
//   5. 不变量 3：未命中的目标骨 pose = identity
//   6. 旋转量正确搬运：源绕单轴转 θ ⟹ 目标对应骨的世界偏差也是绕同一物理轴转 θ
//   7. 根的整体旋转在 Q 里：Q(0) == 两侧整体 rest 朝向差；且被 apply 到输出
//   8. 骨长不参与：两侧只差骨长时结果不受影响
//   9. Pre-condition：尺寸不符 → 崩溃

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

// ── 造一套"迷你两骨骨架"，可指定每骨的 bind 朝向 ──
// 结构:  0 Root (无父) → 1 Child
// 两骨的 rest_offset 都沿 +Y（骨朝上），但 bind_rotation 可分别指定不同朝向，
// 用来模拟"两套 rig 局部坐标系摆放不同"。
struct MiniSkeleton {
    SkeletonType type;
};

MiniSkeleton MakeMini(const geom::Quaternion<float>& root_bind,
                      const geom::Quaternion<float>& child_bind,
                      const std::string& root_name = "Root",
                      const std::string& child_name = "Child") {
    MiniSkeleton m;
    m.type.joints.push_back(MakeJoint(kSkeletonNoParent, 0.0f, 1.0f, 0.0f, root_name));
    m.type.joints.push_back(MakeJoint(0, 0.0f, 0.5f, 0.0f, child_name));
    m.type.bind_rotation.push_back(root_bind);
    m.type.bind_rotation.push_back(child_bind);
    m.type.Validate();
    return m;
}

// 用 Q 的定义**独立**复算一遍（不走被测代码的路径），用于交叉校验。
geom::Quaternion<float> ExpectedQ(const SkeletonType& target, const SkeletonType& source,
                                  int t_idx, int s_idx) {
    const std::vector<geom::Quaternion<float>> tw = RestWorldRotations(target);
    const std::vector<geom::Quaternion<float>> sw = RestWorldRotations(source);
    return (tw[static_cast<size_t>(t_idx)] *
            sw[static_cast<size_t>(s_idx)].Conjugate())
        .Normalized();
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

// 两单位向量的夹角（度）。
float VecAngleDeg(const Vec3f& a, const Vec3f& b) {
    const float la = a.Norm();
    const float lb = b.Norm();
    if (la < 1e-6f || lb < 1e-6f) return 0.0f;
    float c = (a.x() * b.x() + a.y() * b.y() + a.z() * b.z()) / (la * lb);
    c = std::max(-1.0f, std::min(1.0f, c));
    return std::acos(c) * 180.0f / static_cast<float>(M_PI);
}

// ════════════════════════════════════════════════════════════════════════════
//  1. Q 的定义 / 同布局 ⟹ Q ≈ 恒等（最硬门禁）
// ════════════════════════════════════════════════════════════════════════════

// 同布局：两侧 bind 朝向逐骨相同（只差骨长）⟹ Q 应逐骨 ≈ 恒等。
TEST(SkeletonRetargetTest, SameLayoutGivesIdentityQ) {
    const geom::Quaternion<float> bind_root =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), 0.3f);
    const geom::Quaternion<float> bind_child =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(1, 0, 0), -0.7f);

    // 两侧同名同 bind，只有骨长不同。
    MiniSkeleton tgt = MakeMini(bind_root, bind_child);
    MiniSkeleton src = MakeMini(bind_root, bind_child);
    src.type.joints[1].rest_offset = Vec3f(0.0f, 2.5f, 0.0f);  // 骨长不同

    const BindResult r = BuildBind(tgt.type, src.type);

    ASSERT_EQ(r.matched_bone_count, 2);
    for (size_t j = 0; j < r.q_bridge.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(r.q_bridge[j], geom::Quaternion<float>::Identity()), 0.0f,
                    1e-3f)
            << "joint " << j << " 同布局时 Q 应为恒等";
    }
    EXPECT_NEAR(r.q_mean_angle_deg, 0.0f, 1e-3f);
    EXPECT_NEAR(r.q_max_angle_deg, 0.0f, 1e-3f);
}

// 异布局：目标某骨相对源多转 90°（模拟"局部 Z 指大腿侧面 vs 前侧"）⟹ Q 就是那 90°。
TEST(SkeletonRetargetTest, DifferentLayoutGivesThatRotationAsQ) {
    const geom::Quaternion<float> q_extra =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.5f * kPi);

    MiniSkeleton tgt = MakeMini(geom::Quaternion<float>::Identity(), q_extra);
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());

    const BindResult r = BuildBind(tgt.type, src.type);
    ASSERT_EQ(r.matched_bone_count, 2);

    // j=0 两侧一致 ⟹ Q(0) = 恒等
    EXPECT_NEAR(QuatAngleDeg(r.q_bridge[0], geom::Quaternion<float>::Identity()), 0.0f, 1e-3f);
    // j=1 目标多 90° ⟹ Q(1) 应为 90°
    EXPECT_NEAR(QuatAngleDeg(r.q_bridge[1], geom::Quaternion<float>::Identity()), 90.0f, 1e-2f);
    // 且 Q(1) 用定义独立复算一致
    const geom::Quaternion<float> expect = ExpectedQ(tgt.type, src.type, 1, 1);
    EXPECT_NEAR(QuatAngleDeg(r.q_bridge[1], expect), 0.0f, 1e-3f);
}

// Q 表尺寸与目标骨数一致；未命中处为恒等。
TEST(SkeletonRetargetTest, QBridgeSizeAndUnmatchedIdentity) {
    MiniSkeleton tgt = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity(), "RootA", "ChildA");
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity(), "RootB", "ChildB");
    // 两侧骨名完全不同 ⟹ 一个都不命中。
    const BindResult r = BuildBind(tgt.type, src.type);

    EXPECT_EQ(r.q_bridge.size(), tgt.type.joints.size());
    EXPECT_EQ(r.matched_bone_count, 0);
    EXPECT_EQ(r.target_bone_count, 2);
    EXPECT_EQ(r.unmapped_target_bones.size(), 2u);
    for (const auto& q : r.q_bridge) {
        EXPECT_NEAR(QuatAngleDeg(q, geom::Quaternion<float>::Identity()), 0.0f, 1e-6f);
    }
    EXPECT_FALSE(r.rest_alignment.valid);
}

// ════════════════════════════════════════════════════════════════════════════
//  2. 骨名对齐
// ════════════════════════════════════════════════════════════════════════════

TEST(SkeletonRetargetTest, AlignByNameCountsAndTables) {
    // 目标 3 骨（含一个无名包装层），源 2 骨（命中其中 2 个）。
    SkeletonType tgt;
    tgt.joints.push_back(MakeJoint(kSkeletonNoParent, 0, 1, 0, ""));            // 无名
    tgt.joints.push_back(MakeJoint(0, 0, 0.5f, 0, "mixamorig:Hips"));           // 命中
    tgt.joints.push_back(MakeJoint(1, 0, 0.5f, 0, "mixamorig:Missing"));        // 未命中
    tgt.bind_rotation.assign(3, geom::Quaternion<float>::Identity());
    tgt.Validate();

    SkeletonType src;
    src.joints.push_back(MakeJoint(kSkeletonNoParent, 0, 1, 0, "mixamorig:Hips"));
    src.joints.push_back(MakeJoint(0, 0, 0.5f, 0, "mixamorig:Other"));
    src.bind_rotation.assign(2, geom::Quaternion<float>::Identity());
    src.Validate();

    const BindResult r = BuildBind(tgt, src);
    EXPECT_EQ(r.matched_bone_count, 1);            // 只有 Hips 命中
    EXPECT_EQ(r.target_bone_count, 3);
    EXPECT_EQ(r.source_of_target[0], -1);          // 无名 → 不参与
    EXPECT_EQ(r.source_of_target[1], 0);           // Hips → src[0]
    EXPECT_EQ(r.source_of_target[2], -1);          // Missing → 未命中
    // 未命中表**不含**无名骨（它本就无从对位）。
    ASSERT_EQ(r.unmapped_target_bones.size(), 1u);
    EXPECT_EQ(r.unmapped_target_bones[0], "mixamorig:Missing");
    // matches 与目标骨平行，且名字带出来。
    ASSERT_EQ(r.matches.size(), 3u);
    EXPECT_EQ(r.matches[1].name, "mixamorig:Hips");
    EXPECT_EQ(r.matches[1].source_index, 0);
}

// ════════════════════════════════════════════════════════════════════════════
//  3. 不变量 1：源 pose 恒等 ⟹ 输出 pose 恒等
// ════════════════════════════════════════════════════════════════════════════

TEST(SkeletonRetargetTest, IdentitySourcePoseGivesIdentityTargetPose) {
    // 故意用**异布局**（两侧 bind 差 90°），确保"恒等进→恒等出"不是靠布局相同蒙的。
    const geom::Quaternion<float> q_extra =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(1, 0, 0), 0.5f * kPi);
    MiniSkeleton tgt = MakeMini(q_extra, q_extra);
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());

    const SkeletonPose identity = SkeletonPose::Identity(src.type.bone_count());
    const SkeletonPose out = RetargetOnePose(tgt.type, src.type, identity);

    ASSERT_EQ(out.joint_rotation.size(), tgt.type.joints.size());
    for (size_t j = 0; j < out.joint_rotation.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[j], geom::Quaternion<float>::Identity()),
                    0.0f, 1e-3f)
            << "joint " << j << " 源不动 → 目标应保持自己 rest";
    }
    // root_offset 恒 0 —— ⚠️ 这是**固定行为护栏**（实现无条件写 0），不是"算出来对"。
    // 保留它是为了钉住"当前不搬 root-motion"这个约定（M3 接线后本断言应改成新语义）。
    // 它**不能被当作"root 处理正确"的证据**。
    EXPECT_NEAR(out.root_offset.x(), 0.0f, 1e-6f);
    EXPECT_NEAR(out.root_offset.y(), 0.0f, 1e-6f);
    EXPECT_NEAR(out.root_offset.z(), 0.0f, 1e-6f);
}

// ════════════════════════════════════════════════════════════════════════════
//  4. 不变量 2：偏差逐骨一致（deviation_T == deviation_S）
// ════════════════════════════════════════════════════════════════════════════

TEST(SkeletonRetargetTest, DeviationIsPreservedPerBone) {
    const geom::Quaternion<float> q_extra =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.25f * kPi);
    MiniSkeleton tgt = MakeMini(q_extra, q_extra);
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());

    // 源：两骨各转一个明显角度。
    SkeletonPose src_pose = SkeletonPose::Identity(2);
    src_pose.joint_rotation[0] =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), 0.4f);
    src_pose.joint_rotation[1] =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), -0.9f);

    const SkeletonPose out = RetargetOnePose(tgt.type, src.type, src_pose);

    // 命中骨：**偏差**（header 定义 Rb⁻¹W）应逐骨精确一致。
    for (int j = 0; j < 2; ++j) {
        const Deviation ds = ComputeDeviation(src.type, src_pose, j);
        const Deviation dt = ComputeDeviation(tgt.type, out, j);
        EXPECT_NEAR(QuatAngleDeg(ds.q, dt.q), 0.0f, 1e-2f)
            << "joint " << j << " 偏差应逐骨一致（跨两侧 rest 系）";
        EXPECT_NEAR(ds.angle_deg, dt.angle_deg, 1e-2f) << "joint " << j << " 偏差角一致";
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  5. 不变量 3：未命中的目标骨 pose = identity
// ════════════════════════════════════════════════════════════════════════════

TEST(SkeletonRetargetTest, UnmatchedBoneStaysAtRest) {
    // 目标 3 骨：Root 无名（不参与）、A、B；源只有 A ⟹ B 未命中。
    SkeletonType tgt;
    tgt.joints.push_back(MakeJoint(kSkeletonNoParent, 0, 1, 0, ""));
    tgt.joints.push_back(MakeJoint(0, 0, 0.5f, 0, "A"));
    tgt.joints.push_back(MakeJoint(1, 0, 0.5f, 0, "B"));
    tgt.bind_rotation.assign(3, geom::Quaternion<float>::Identity());
    tgt.Validate();

    SkeletonType src;
    src.joints.push_back(MakeJoint(kSkeletonNoParent, 0, 1, 0, "A"));
    src.joints.push_back(MakeJoint(0, 0, 0.5f, 0, "Z"));
    src.bind_rotation.assign(2, geom::Quaternion<float>::Identity());
    src.Validate();

    SkeletonPose src_pose = SkeletonPose::Identity(2);
    src_pose.joint_rotation[0] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), 1.0f);
    src_pose.joint_rotation[1] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.6f);

    const SkeletonPose out = RetargetOnePose(tgt, src, src_pose);

    // A（index 1）被驱动：非恒等。
    EXPECT_GT(QuatAngleDeg(out.joint_rotation[1], geom::Quaternion<float>::Identity()), 1.0f);
    // B（index 2，未命中）保持 rest = 恒等。
    EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[2], geom::Quaternion<float>::Identity()), 0.0f,
                1e-4f)
        << "未命中的目标骨必须保持自身 rest";
}

// ════════════════════════════════════════════════════════════════════════════
//  6. 旋转量正确搬运（物理轴不变）
// ════════════════════════════════════════════════════════════════════════════

// 源某骨绕**世界 Z** 转 30° ⟹ 目标对应骨的**世界偏差**也应是绕世界 Z 转 30°
// （"同一个物理旋转"，与各自局部帧无关）。这是重定向最本质的语义断言。
TEST(SkeletonRetargetTest, PhysicalAxisIsPreserved) {
    const geom::Quaternion<float> q_extra =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(1, 0, 0), 0.5f * kPi);
    MiniSkeleton tgt = MakeMini(geom::Quaternion<float>::Identity(), q_extra);
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());

    // 源 j=1（局部系 = 世界系，因其 bind 恒等）绕世界 Z 转 30°。
    SkeletonPose src_pose = SkeletonPose::Identity(2);
    const float theta = 30.0f * kPi / 180.0f;
    src_pose.joint_rotation[1] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 0, 1), theta);

    const SkeletonPose out = RetargetOnePose(tgt.type, src.type, src_pose);

    // 源 j=1 的物理旋转 = 绕世界 Z 转 theta（其 bind 恒等 ⇒ 局部系 = 世界系）。
    // 目标 j=1 的 bind 绕 X 转了 90°，故其**局部系里的四元数**长得不一样 ——
    // 但"偏差"（Rb⁻¹W）按 header 定义应精确相等。这是本测试的核心断言。
    const Deviation ds = ComputeDeviation(src.type, src_pose, 1);
    const Deviation dt = ComputeDeviation(tgt.type, out, 1);

    EXPECT_NEAR(ds.angle_deg, 30.0f, 1e-2f) << "源的偏差角应就是 theta=30°";
    EXPECT_NEAR(dt.angle_deg, 30.0f, 1e-2f) << "目标的偏差角也应是 30°";
    EXPECT_NEAR(QuatAngleDeg(ds.q, dt.q), 0.0f, 1e-2f)
        << "偏差应精确一致（两侧局部帧不同也不影响）";

    // 物理可观测量交叉校验："相对各自 rest 的骨段方向"应一致。
    // 骨段方向 = 该骨的 rest_offset 方向 (+Y) 被"偏差"旋转后的结果。
    // （两侧 rest 骨轴本身不同，故只能比相对自己的变化量。）
    const Vec3f bone_axis(0.0f, 1.0f, 0.0f);
    const Vec3f rel_src = geom::RotateVector(ds.q, bone_axis);
    const Vec3f rel_tgt = geom::RotateVector(dt.q, bone_axis);
    EXPECT_NEAR((rel_src - rel_tgt).Norm(), 0.0f, 1e-3f)
        << "相对各自 rest 的骨段方向应一致（同一个物理姿态）";
}

// ════════════════════════════════════════════════════════════════════════════
//  7. 根的整体旋转在 Q 里
// ════════════════════════════════════════════════════════════════════════════

// 源骨架整体绕 Y 转 90°（模拟"源朝 +Z、目标朝 +X"）⟹ Q(0) 就是那个 90°。
// ⚠️ 注意 Q(0) 捕获的是**两侧 rest 的整体朝向差**；apply 后目标保持**自己的** rest
//   朝向（源 pose 恒等 ⇒ 目标 pose 恒等，不变量 1）。所以本测试**断言 Q(0) 的值**，
//   **不**断言"目标根世界朝向 == 源根世界朝向"（那是错的目标函数 —— 见文件头）。
//   Q(0) 的用途是：消费者若想让两侧"面朝同一边"，**单独**把它用在放置层。
TEST(SkeletonRetargetTest, RootOrientationDifferenceLivesInQ0) {
    const geom::Quaternion<float> q90 =
        geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.5f * kPi);

    // 源根带 90°，目标根恒等。
    MiniSkeleton tgt = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());
    MiniSkeleton src = MakeMini(q90, geom::Quaternion<float>::Identity());

    const BindResult r = BuildBind(tgt.type, src.type);
    ASSERT_EQ(r.matched_bone_count, 2);

    // Q(0) = 两侧整体 rest 朝向差 = 90°。
    EXPECT_NEAR(QuatAngleDeg(r.q_bridge[0], geom::Quaternion<float>::Identity()), 90.0f, 1e-2f);
    // 单列出来的 rest_alignment 与 Q(0) 一致（单一真值，不是另算的）。
    EXPECT_TRUE(r.rest_alignment.valid);
    EXPECT_NEAR(QuatAngleDeg(r.rest_alignment.rotation, r.q_bridge[0]), 0.0f, 1e-6f);
    EXPECT_NEAR(r.rest_alignment.angle_deg, 90.0f, 1e-2f);

    // 不变量 1（与 Q(0) 并存不悖）：源 pose 恒等 ⟹ 目标 pose 恒等（保持自己的 rest）。
    // ⚠️ 注意 Q(0) 捕获的是**两侧 rest 的朝向差**，不是"把目标根摆成源根"。
    //    源不动 ⇒ 偏差恒等 ⇒ 目标也保持自己的 rest —— 这正是设计意图。
    const SkeletonPose out = RetargetOnePose(tgt.type, src.type,
                                            SkeletonPose::Identity(2));
    for (size_t j = 0; j < out.joint_rotation.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[j], geom::Quaternion<float>::Identity()),
                    0.0f, 1e-3f);
    }
    // 两侧的**偏差**都是恒等（都没动）——这是"同一物理姿态"的正确判据。
    for (int j = 0; j < 2; ++j) {
        const Deviation ds = ComputeDeviation(src.type, SkeletonPose::Identity(2), j);
        const Deviation dt = ComputeDeviation(tgt.type, out, j);
        EXPECT_NEAR(ds.angle_deg, dt.angle_deg, 1e-3f) << "joint " << j << " 都未动";
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  8. 骨长不参与
// ════════════════════════════════════════════════════════════════════════════

// 两侧只差骨长（bind 相同）⟹ 输出应与"骨长相同"时**完全一致**。
// ⚠️ 这是**护栏（regression guard）**，不是发现型测试：当前实现根本不读 rest_offset，
//   故它天然恒过。它的价值在于——**将来若有人把 rest_offset 混进重定向数学，它会报错**。
TEST(SkeletonRetargetTest, BoneLengthDoesNotAffectResult) {
    SkeletonPose src_pose = SkeletonPose::Identity(2);
    src_pose.joint_rotation[1] = geom::Quaternion<float>::FromAxisAngle(Vec3f(0, 1, 0), 0.8f);

    // A：两侧骨长相同
    MiniSkeleton tgt_a = MakeMini(geom::Quaternion<float>::Identity(),
                                  geom::Quaternion<float>::Identity());
    MiniSkeleton src_a = MakeMini(geom::Quaternion<float>::Identity(),
                                  geom::Quaternion<float>::Identity());
    const SkeletonPose out_a = RetargetOnePose(tgt_a.type, src_a.type, src_pose);

    // B：目标骨长改成 7 倍、源改成 0.3 倍
    MiniSkeleton tgt_b = MakeMini(geom::Quaternion<float>::Identity(),
                                  geom::Quaternion<float>::Identity());
    MiniSkeleton src_b = MakeMini(geom::Quaternion<float>::Identity(),
                                  geom::Quaternion<float>::Identity());
    tgt_b.type.joints[1].rest_offset = Vec3f(0.0f, 3.5f, 0.0f);
    src_b.type.joints[1].rest_offset = Vec3f(0.0f, 0.15f, 0.0f);
    const SkeletonPose out_b = RetargetOnePose(tgt_b.type, src_b.type, src_pose);

    ASSERT_EQ(out_a.joint_rotation.size(), out_b.joint_rotation.size());
    for (size_t j = 0; j < out_a.joint_rotation.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(out_a.joint_rotation[j], out_b.joint_rotation[j]), 0.0f, 1e-4f)
            << "joint " << j << "：骨长不应影响重定向结果";
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  9. Mixamo23 自洽性（真实骨架规模上的门禁）
// ════════════════════════════════════════════════════════════════════════════

// 同一套骨架（构造两次）互相对位 ⟹ 命中的骨全恒等 Q、任意 pose 原样复现。
// ⚠️ Mixamo23 的 0 号骨是**无名包装层 Root**（`name == ""`）—— 按约定无名骨不参与对位，
//   故命中 22/23（不是 23）。这**不是缺陷**，是本文件"空名不参与"约定的直接结果。
TEST(SkeletonRetargetTest, Mixamo23SelfRetargetIsExact) {
    const SkeletonType target = Mixamo23Skeleton(1.60f);
    const SkeletonType source = Mixamo23Skeleton(1.60f);

    const BindResult r = BuildBind(target, source);
    // 0 号骨无名 ⇒ 少一个命中。
    EXPECT_EQ(r.target_bone_count, 23);
    EXPECT_EQ(r.matched_bone_count, 22);
    ASSERT_EQ(r.unmapped_target_bones.size(), 0u) << "无名骨不进未命中表";
    EXPECT_EQ(r.source_of_target[0], -1) << "无名包装层 Root 不参与对位";
    // 除 0 号（未命中）外，其余骨的 Q 全恒等（同布局）。
    // ⚠️ 注意：两侧是**同一套骨架**，故 Rb_T == Rb_S，Q 恒等是**构造使然**（非发现）。
    //   它验证的是"两侧 rest 世界朝向被一致地算出来"（若两遍计算不自洽就会报错），价值有限
    //   但便宜。真正的发现型断言是下面的 matched==22 与"未命中骨保持 rest"。
    for (size_t j = 1; j < r.q_bridge.size(); ++j) {
        EXPECT_NEAR(QuatAngleDeg(r.q_bridge[j], geom::Quaternion<float>::Identity()), 0.0f,
                    1e-1f)
            << "joint " << j << "（同布局）Q 应为恒等";
    }

    // 造一个有内容的 pose（几根骨各转不同量）——用真实骨架验证"原样复现"。
    SkeletonPose pose = SkeletonPose::Identity(source.bone_count());
    const int n = source.bone_count();
    for (int j = 1; j < n; ++j) {  // 跳过 0（它不参与对位，无法被"复现"）
        const float a = 0.1f * static_cast<float>(j + 1);
        const Vec3f axis((j % 3 == 0) ? 1.0f : 0.0f, (j % 3 == 1) ? 1.0f : 0.0f,
                         (j % 3 == 2) ? 1.0f : 0.0f);
        pose.joint_rotation[static_cast<size_t>(j)] =
            geom::Quaternion<float>::FromAxisAngle(axis, a);
    }

    const SkeletonPose out = RetargetOnePose(target, source, pose);
    // 被驱动的骨（1..n-1）：同布局 ⇒ 局部 pose 原样复现（Q=恒等）。
    for (int j = 1; j < n; ++j) {
        EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[static_cast<size_t>(j)],
                                 pose.joint_rotation[static_cast<size_t>(j)]),
                    0.0f, 5e-2f)
            << "joint " << j << "：同骨架重定向应原样复现 pose";
    }
    // 未命中的 0 号骨：保持自身 rest（pose = identity），即使源把它转了。
    EXPECT_NEAR(QuatAngleDeg(out.joint_rotation[0], geom::Quaternion<float>::Identity()), 0.0f,
                1e-4f)
        << "未命中的 Root 必须保持自身 rest";
}

// ════════════════════════════════════════════════════════════════════════════
//  10. Pre-condition 崩溃
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════
//  11. 人体随动系 EstimateBodyFrame
// ════════════════════════════════════════════════════════════════════════

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

geom::Quaternion<float> RotYDeg(float deg) {
    return geom::Quaternion<float>::FromAxisAngle(Vec3f(0.0f, 1.0f, 0.0f),
                                                  deg * kPi / 180.0f);
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

// T-pose 面朝 +Z（左手 +X、右手 −X）⇒ M = 恒等，三轴 = (X, Y, Z)。
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

TEST(SkeletonRetargetTest, WrongSourcePoseSizeDies) {
    MiniSkeleton tgt = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());
    MiniSkeleton src = MakeMini(geom::Quaternion<float>::Identity(),
                                geom::Quaternion<float>::Identity());
    const BindResult r = BuildBind(tgt.type, src.type);

    SkeletonPose bad;  // bone_count = 0，joint_rotation 非空但尺寸不符 → 应崩溃
    bad.bone_count = 1;
    bad.joint_rotation.resize(1);

    SkeletonPose out;
    EXPECT_DEATH(RetargetPose(r, bad, &out), "source_pose 尺寸");
}

}  // namespace
}  // namespace jpov

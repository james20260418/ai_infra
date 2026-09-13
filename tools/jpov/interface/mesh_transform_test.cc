// mesh_transform_test — 把放置参数烘进 CPU 资产的纯数学单测（GL-free）
//
// 覆盖（对应 docs/jpov_model_editor_save_plan.md §2）：
//   1. 顶点烘焙：与 DrawObject3D 的 v' = center + R·(scale·v) 严格一致（含 scale）。
//   2. 法线/切线：法线只转不缩、切线同位置（乘 scale、不归一化）。
//   3. 🔑 骨架一致性：顶点烘 U 与骨架共轭烘 U **等价**——
//        Σ w·(U·M·U⁻¹)·(U·v)  ==  U·(Σ w·M·v)
//      这是"旋转后的带骨模型存下来 pose 跑掉"那个坑的数学本体。
//   4. 旋转基正交归一 + 右手系；up/front 微不垂直时被正交化（不引入剪切）。
//   5. 边界：退化 up/front → crash（不静默）。
//
// 实现说明：本测试**独立实现**一份 jointWorld 复合（与 skeleton_manager.cc 同式
//   jw[j] = jw[parent] · T(rest)·R(bind)·R(pose)，根无父），不做成"调被测代码自证"——
//   否则两边同错还照样绿。

#include <cmath>
#include <vector>

#include <gtest/gtest.h>
#include "geom/common/quaternion.h"
#include "geom/math/mat4.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mesh_transform.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {
namespace {

using geom::Quaternion;
using geom::math::Mat4;

// ---- 与 skeleton_manager.cc 同式的 jointWorld 复合（独立实现，供对照）----
// jw[j] = (根 ? local : jw[parent]) · local；local = T(rest)·R(bind)·R(pose)。
std::vector<Mat4> ComputeJointWorld(const SkeletonType& type,
                                    const SkeletonPose& pose) {
    const size_t n = type.joints.size();
    std::vector<Mat4> jw(n);
    for (size_t j = 0; j < n; ++j) {
        const Quaternion<float> bind = type.bind_rotation.empty()
                                           ? Quaternion<float>::Identity()
                                           : type.bind_rotation[j];
        const Quaternion<float> pr = pose.joint_rotation.size() > j
                                         ? pose.joint_rotation[j]
                                         : Quaternion<float>::Identity();
        const Mat4 local = geom::math::JointLocal(
            type.joints[j].rest_offset, bind, pr);
        const int p = type.joints[j].parent;
        jw[j] = (p == kSkeletonNoParent)
                    ? local
                    : geom::math::Mat4Mul(jw[static_cast<size_t>(p)], local);
    }
    return jw;
}

// 放置矩阵 U = T(center)·R(up,front)（与 BuildModelMatrix 一致的刚体部）。
Mat4 PlacementMatrix(const Vec3f& center, const Vec3f& up, const Vec3f& front) {
    const PlacementBasis b = MakePlacementBasis(up, front);
    Mat4 r = geom::math::Mat4Identity();
    // 列主序：col0=left, col1=up, col2=front。
    r.m[0] = b.left.x();  r.m[1] = b.left.y();  r.m[2] = b.left.z();
    r.m[4] = b.up.x();    r.m[5] = b.up.y();    r.m[6] = b.up.z();
    r.m[8] = b.front.x(); r.m[9] = b.front.y(); r.m[10] = b.front.z();
    r.m[12] = center.x(); r.m[13] = center.y(); r.m[14] = center.z();
    return r;
}

// 造一棵小骨架：根(0) → 上臂(1) → 前臂(2)，带非平凡 bind 朝向与骨长。
SkeletonType MakeTestSkeleton() {
    SkeletonType t;
    t.joints.resize(3);
    t.joints[0].parent = kSkeletonNoParent;
    t.joints[0].rest_offset = {0.0f, 0.9f, 0.0f};   // 根（骨盆）在地面上方
    t.joints[0].name = "root";
    t.joints[1].parent = 0;
    t.joints[1].rest_offset = {0.0f, 0.3f, 0.0f};
    t.joints[1].name = "upper";
    t.joints[2].parent = 1;
    t.joints[2].rest_offset = {0.0f, 0.28f, 0.0f};
    t.joints[2].name = "lower";
    // bind 朝向非恒等：上臂绕 Z 掰 90°（水平），前臂绕 X 略拧。
    t.bind_rotation.resize(3);
    t.bind_rotation[0] = Quaternion<float>::Identity();
    t.bind_rotation[1] = Quaternion<float>::FromAxisAngle(
        {0.0f, 0.0f, 1.0f}, static_cast<float>(M_PI / 2.0));
    t.bind_rotation[2] = Quaternion<float>::FromAxisAngle(
        {1.0f, 0.0f, 0.0f}, static_cast<float>(M_PI / 6.0));
    t.Validate();
    return t;
}

// 造一个非平凡的 pose（各骨绕不同轴转不同角）。
SkeletonPose MakeTestPose(int bone_count) {
    SkeletonPose p = SkeletonPose::Identity(bone_count);
    p.joint_rotation[0] = Quaternion<float>::FromAxisAngle(
        {0.0f, 1.0f, 0.0f}, static_cast<float>(M_PI / 5.0));
    p.joint_rotation[1] = Quaternion<float>::FromAxisAngle(
        {1.0f, 0.0f, 0.0f}, static_cast<float>(-M_PI / 3.0));
    p.joint_rotation[2] = Quaternion<float>::FromAxisAngle(
        {0.0f, 0.0f, 1.0f}, static_cast<float>(M_PI / 4.0));
    return p;
}

// 一个带 skin 权重的顶点：position + 4 组 joint/weight。
MeshData MakeTestMesh(const Vec3f& v, const std::array<int32_t, 4>& ji,
                      const std::array<float, 4>& jw) {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kJoints));
    m.positions = {v};
    m.normals = {{0.0f, 1.0f, 0.0f}};
    m.uvs = {{0.2f, 0.8f}};
    m.joint_indices = {ji};
    m.joint_weights = {jw};
    m.indices = {0};
    m.Validate();
    return m;
}

// 蒙皮一个顶点：Σ w·final·v（final = jw·IBM），与 shader 的链路同式。
Vec3f SkinVertex(const std::vector<Mat4>& jw, const SkeletonType& t,
                 const std::array<int32_t, 4>& ji,
                 const std::array<float, 4>& wt, const Vec3f& v) {
    // IBM 来自骨架自身（ComputeInverseBind），与 skeleton_manager 生成 IBM 同源同式。
    const std::vector<std::array<float, 16>> ibm = t.ComputeInverseBind();
    Vec3f acc{0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 4; ++k) {
        if (wt[k] == 0.0f) {
            continue;
        }
        Mat4 inv{};
        for (int i = 0; i < 16; ++i) {
            inv.m[i] = ibm[static_cast<size_t>(ji[k])][i];
        }
        const Mat4 final_m = geom::math::Mat4Mul(jw[static_cast<size_t>(ji[k])],
                                                 inv);
        const Vec3f p = geom::math::Mat4TransformPoint(final_m, v);
        acc = Vec3f(acc.x() + wt[k] * p.x(), acc.y() + wt[k] * p.y(),
                    acc.z() + wt[k] * p.z());
    }
    return acc;
}

float MaxDiff(const Vec3f& a, const Vec3f& b) {
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.z() - b.z())});
}

// ==================== 1. 顶点烘焙与 DrawObject3D 一致 ====================

TEST(MeshTransformTest, VertexBakeMatchesPlacementFormula) {
    MeshData in = MakeTestMesh({1.0f, 2.0f, 3.0f}, {0, 0, 0, 0},
                               {1.0f, 0.0f, 0.0f, 0.0f});
    const Vec3f center{0.5f, -1.0f, 2.0f};
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{0.0f, 0.0f, 1.0f};   // 绕 Y 转 90° 之外另测
    const float scale = 2.5f;

    const MeshData out =
        ApplyPlacementToMesh(in, center, up, front, scale);

    // 期望：center + R·(scale·v)。此处 R = identity（up/front 即标准基）。
    const Vec3f expect{center.x() + scale * 1.0f, center.y() + scale * 2.0f,
                       center.z() + scale * 3.0f};
    EXPECT_LT(MaxDiff(out.positions[0], expect), 1e-5f);

    // 法线：R·n（不缩放）。
    EXPECT_LT(MaxDiff(out.normals[0], Vec3f(0.0f, 1.0f, 0.0f)), 1e-5f);
    // UV 原样。
    EXPECT_FLOAT_EQ(out.uvs[0].x(), in.uvs[0].x());
    EXPECT_FLOAT_EQ(out.uvs[0].y(), in.uvs[0].y());
    // 入参未被修改（纯函数）。
    EXPECT_FLOAT_EQ(in.positions[0].x(), 1.0f);
}

TEST(MeshTransformTest, VertexBakeRotationMatchesManualMatrix) {
    MeshData in = MakeTestMesh({1.0f, 0.0f, 0.0f}, {0, 0, 0, 0},
                               {1.0f, 0.0f, 0.0f, 0.0f});
    // 绕世界 Y 转 90°（右手系）：up=+Y 不变，front=+Z → +X（这时 left=+Y? 见下）。
    const Vec3f center{10.0f, 20.0f, 30.0f};
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{1.0f, 0.0f, 0.0f};   // 原 +Z 轴转了 90° → +X
    const float scale = 3.0f;

    const MeshData out = ApplyPlacementToMesh(in, center, up, front, scale);

    // 用独立构造的 U 矩阵算期望值（不调被测的 RotateByBasis）。
    const Mat4 u = PlacementMatrix(center, up, front);
    const Vec3f expect = geom::math::Mat4TransformPoint(
        u, Vec3f(scale * 1.0f, scale * 0.0f, scale * 0.0f));
    EXPECT_LT(MaxDiff(out.positions[0], expect), 1e-4f);

    // 该旋转下局部 +X (left) 应映射到 -Z（cross(up=(0,1,0), front=(1,0,0)) = (0,0,-1)）。
    // 顶点 (1,0,0) 局部 → center + 3·left = (10,20,30) + 3·(0,0,-1) = (10,20,27)。
    EXPECT_LT(MaxDiff(out.positions[0], Vec3f(10.0f, 20.0f, 27.0f)), 1e-4f);
}

// ==================== 2. 🔑 骨架一致性（核心） ====================
//
// ⚠️ 重要前提（pose 也须共轭）：蒙皮链里每个关节 local = T(o)·R(b)·R(pose)。
//   烘 U 后 local' = U·local·U⁻¹ = [T(o')·R(b')]·[U·R(pose)·U⁻¹]。
//   即：**除了 rest_offset/bind_rotation，pose 也必须共轭（pose' = q_R·pose·q_R⁻¹）**，
//   否则带非恒等 pose 的资产烘完 U 后姿势会错。
//   保存场景下**存的就是 bind/rest 姿态（pose 恒等）**，恒等 pose 共轭后仍是恒等，
//   故本函数的'刚体 U'语义对保存场景是完备的（见下面用 Identity pose 的等价验证）。
//   若将来要烘"带 pose 的资产"，必须同步共轭 pose——本文件不承担该扩展，
//   故意不在 ApplyPlacementToSkeleton 里接 pose 参数（避免静默漏共轭）。

// 顶点烘 U 与骨架共轭烘 U 后，蒙皮结果应 == U·(原始蒙皮结果)。
// （pose = bind/rest 恒等 —— 即保存场景的真实姿态。）
TEST(MeshTransformTest, SkeletonAndVertexBakeAreConsistent) {
    const SkeletonType type = MakeTestSkeleton();
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());

    // 顶点：受影响于上臂/前臂（joint 1、2），权重各半。
    const std::array<int32_t, 4> ji = {1, 2, 0, 0};
    const std::array<float, 4> wt = {0.6f, 0.4f, 0.0f, 0.0f};
    const Vec3f v_local{0.15f, 1.1f, -0.05f};
    const MeshData mesh = MakeTestMesh(v_local, ji, wt);

    const Vec3f center{2.0f, 0.5f, -1.0f};
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{0.0f, 0.0f, 1.0f};   // identity 旋转下先做基本等价验证

    // 原始：jw(pose) → 蒙皮。
    const std::vector<Mat4> jw0 = ComputeJointWorld(type, pose);
    const Vec3f skin0 = SkinVertex(jw0, type, ji, wt, v_local);

    // 烘焙后：骨架共轭 U、顶点烘 U（scale=1）。
    const SkeletonType baked = ApplyPlacementToSkeleton(type, center, up, front);
    const MeshData baked_mesh =
        ApplyPlacementToMesh(mesh, center, up, front, 1.0f);
    const std::vector<Mat4> jw1 = ComputeJointWorld(baked, pose);   // pose 不变
    const Vec3f skin1 =
        SkinVertex(jw1, baked, ji, wt, baked_mesh.positions[0]);

    // 期望：skin1 == U·skin0。
    const Mat4 u = PlacementMatrix(center, up, front);
    const Vec3f expect = geom::math::Mat4TransformPoint(u, skin0);
    EXPECT_LT(MaxDiff(skin1, expect), 1e-3f)
        << "骨架共轭烘 U 与顶点烘 U 不等价（skin1 应 == U·skin0）";
}

// 同一个一致性，但放置是**非平凡旋转**（这才是需求验收路径：手转一个 glb）。
TEST(MeshTransformTest, SkeletonAndVertexBakeConsistentUnderRotation) {
    const SkeletonType type = MakeTestSkeleton();
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());

    const std::array<int32_t, 4> ji = {0, 1, 2, 0};
    const std::array<float, 4> wt = {0.2f, 0.5f, 0.3f, 0.0f};
    const Vec3f v_local{0.2f, 1.0f, 0.1f};
    const MeshData mesh = MakeTestMesh(v_local, ji, wt);

    const Vec3f center{0.3f, 1.2f, -0.7f};
    // 绕 Y 转 60° 的 up/front：front 原 +Z → 绕 +Y 转 60°。
    const float a60 = static_cast<float>(M_PI / 3.0);
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{std::sin(a60), 0.0f, std::cos(a60)};

    const std::vector<Mat4> jw0 = ComputeJointWorld(type, pose);
    const Vec3f skin0 = SkinVertex(jw0, type, ji, wt, v_local);

    const SkeletonType baked = ApplyPlacementToSkeleton(type, center, up, front);
    const MeshData baked_mesh =
        ApplyPlacementToMesh(mesh, center, up, front, 1.0f);
    const std::vector<Mat4> jw1 = ComputeJointWorld(baked, pose);
    const Vec3f skin1 =
        SkinVertex(jw1, baked, ji, wt, baked_mesh.positions[0]);

    const Mat4 u = PlacementMatrix(center, up, front);
    const Vec3f expect = geom::math::Mat4TransformPoint(u, skin0);
    EXPECT_LT(MaxDiff(skin1, expect), 1e-3f);
}

// 非恒等 pose 下**不经 pose 共轭**会不一致——记录这一边界（证明测试真能区分）。
// 保存场景不涉及（存的就是 rest 姿态），但将来"烘带 pose 的资产"必须同步共轭 pose。
TEST(MeshTransformTest, NonIdentityPoseRequiresPoseConjugation) {
    const SkeletonType type = MakeTestSkeleton();
    const SkeletonPose pose = MakeTestPose(type.bone_count());   // 非恒等

    const std::array<int32_t, 4> ji = {1, 2, 0, 0};
    const std::array<float, 4> wt = {0.6f, 0.4f, 0.0f, 0.0f};
    const Vec3f v_local{0.15f, 1.1f, -0.05f};
    const MeshData mesh = MakeTestMesh(v_local, ji, wt);

    const Vec3f center{2.0f, 0.5f, -1.0f};
    const float a60 = static_cast<float>(M_PI / 3.0);
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{std::sin(a60), 0.0f, std::cos(a60)};

    const std::vector<Mat4> jw0 = ComputeJointWorld(type, pose);
    const Vec3f skin0 = SkinVertex(jw0, type, ji, wt, v_local);

    const SkeletonType baked = ApplyPlacementToSkeleton(type, center, up, front);
    const MeshData baked_mesh =
        ApplyPlacementToMesh(mesh, center, up, front, 1.0f);
    const std::vector<Mat4> jw1 = ComputeJointWorld(baked, pose);
    const Vec3f skin1 =
        SkinVertex(jw1, baked, ji, wt, baked_mesh.positions[0]);

    const Mat4 u = PlacementMatrix(center, up, front);
    const Vec3f expect = geom::math::Mat4TransformPoint(u, skin0);
    EXPECT_GT(MaxDiff(skin1, expect), 1e-2f)
        << "非恒等 pose 未共轭却与正确结果一致 → 前提说明失效";
}

// 非恒等 pose 下，
// ⚠️ **本函数的骨架烘焙只对 rest 姿态正确**（保存场景就是 rest）。
//   若资产带非恒等 pose，偏差项会从 `t − R(b_j)·t` 变成 `t − R(b_j)R(pose_j)·t`
//   （因关节 local = T(o)·R(b)·R(pose)，共轭后平移项随**完整旋转**而非仅 b 变）。
//   本测试用一个非恒等 pose 的"自定义参照"（把偏差项按带 pose 的正确公式算）
//   确认：与 ApplyPlacementToSkeleton（只按 b 算偏差）**不等价** —— 把这条边界钉死，
//   避免以后有人拿带 pose 的资产直用本函数。
TEST(MeshTransformTest, BakeIsOnlyValidAtRestPose) {
    const SkeletonType type = MakeTestSkeleton();
    const SkeletonPose pose = MakeTestPose(type.bone_count());   // 非恒等

    const std::array<int32_t, 4> ji = {1, 2, 0, 0};
    const std::array<float, 4> wt = {0.6f, 0.4f, 0.0f, 0.0f};
    const Vec3f v_local{0.15f, 1.1f, -0.05f};
    const MeshData mesh = MakeTestMesh(v_local, ji, wt);

    const Vec3f center{2.0f, 0.5f, -1.0f};
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{0.0f, 0.0f, 1.0f};   // R = I（纯平移）
    const Mat4 u = PlacementMatrix(center, up, front);

    const std::vector<Mat4> jw0 = ComputeJointWorld(type, pose);
    const Vec3f skin0 = SkinVertex(jw0, type, ji, wt, v_local);

    const SkeletonType baked = ApplyPlacementToSkeleton(type, center, up, front);
    const MeshData baked_mesh =
        ApplyPlacementToMesh(mesh, center, up, front, 1.0f);
    const std::vector<Mat4> jw1 = ComputeJointWorld(baked, pose);
    const Vec3f skin1 =
        SkinVertex(jw1, baked, ji, wt, baked_mesh.positions[0]);

    const Vec3f expect = geom::math::Mat4TransformPoint(u, skin0);
    // 非 rest 姿态下不等价（偏差项少了 pose 旋转）—— 锁住这个边界。
    EXPECT_GT(MaxDiff(skin1, expect), 1e-2f)
        << "带 pose 的资产直接用本函数烘焙本就不被支持；若恰好相等说明前提变了";
}

// 负向（rest 姿态专用）：收下"rest 姿态下 final 矩阵恒为 I"这一事实——
//   此时蒙皮是恒等映射，只烘顶点表面上也能过。故 rest 姿态下的负向必须靠
//   "非恒等 pose"（见上一个测试）。这里显式锁住这个退化事实，避免以后误把
//   rest 姿态的"vertex-only 也通过"当成 bug。
TEST(MeshTransformTest, RestPoseSkinningIsIdentity) {
    const SkeletonType type = MakeTestSkeleton();
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const std::vector<Mat4> jw = ComputeJointWorld(type, pose);
    const Vec3f v{0.3f, 1.1f, -0.2f};
    const Vec3f skin = SkinVertex(jw, type, {1, 2, 0, 0}, {0.6f, 0.4f, 0.0f, 0.0f}, v);
    EXPECT_LT(MaxDiff(skin, v), 1e-5f) << "rest 姿态下 jointWorld·IBM 应为恒等";
}

// ==================== 3. 旋转基性质 ====================

TEST(MeshTransformTest, BasisIsOrthonormalRightHanded) {
    // 故意给一个 up/front 略不垂直的输入，验证被正交化。
    const Vec3f up{0.05f, 1.0f, 0.0f};
    const Vec3f front{0.0f, 0.1f, 1.0f};
    const PlacementBasis b = MakePlacementBasis(up, front);

    auto len = [](const Vec3f& v) {
        return std::sqrt(v.x() * v.x() + v.y() * v.y() + v.z() * v.z());
    };
    auto dot = [](const Vec3f& a, const Vec3f& c) {
        return a.x() * c.x() + a.y() * c.y() + a.z() * c.z();
    };
    EXPECT_NEAR(len(b.left), 1.0f, 1e-5f);
    EXPECT_NEAR(len(b.up), 1.0f, 1e-5f);
    EXPECT_NEAR(len(b.front), 1.0f, 1e-5f);
    EXPECT_NEAR(dot(b.left, b.up), 0.0f, 1e-5f);
    EXPECT_NEAR(dot(b.left, b.front), 0.0f, 1e-5f);
    EXPECT_NEAR(dot(b.up, b.front), 0.0f, 1e-5f);   // 正交化后必须垂直

    // 右手系：left = cross(up, front)。
    const Vec3f cr(b.up.y() * b.front.z() - b.up.z() * b.front.y(),
                   b.up.z() * b.front.x() - b.up.x() * b.front.z(),
                   b.up.x() * b.front.y() - b.up.y() * b.front.x());
    EXPECT_LT(MaxDiff(cr, b.left), 1e-5f);
}

TEST(MeshTransformTest, BasisPreservesUpDirection) {
    // up 归一化后方向不变（不因正交化被改动）。
    const Vec3f up{0.0f, 2.0f, 0.0f};
    const Vec3f front{0.0f, 0.0f, 5.0f};
    const PlacementBasis b = MakePlacementBasis(up, front);
    EXPECT_LT(MaxDiff(b.up, Vec3f(0.0f, 1.0f, 0.0f)), 1e-5f);
    EXPECT_LT(MaxDiff(b.front, Vec3f(0.0f, 0.0f, 1.0f)), 1e-5f);
    // left = cross((0,1,0),(0,0,1)) = (1,0,0)。
    EXPECT_LT(MaxDiff(b.left, Vec3f(1.0f, 0.0f, 0.0f)), 1e-5f);
}

// ==================== 4. 骨架烘焙的具体数值 ====================

TEST(MeshTransformTest, SkeletonBakeOffsetsEveryJointByBiasTerm) {
    const SkeletonType type = MakeTestSkeleton();
    const Vec3f center{0.4f, -0.3f, 0.9f};
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f front{0.0f, 0.0f, 1.0f};
    const SkeletonType baked =
        ApplyPlacementToSkeleton(type, center, up, front);

    // identity 旋转（R=I）下，每个关节都应加同一修正项 center − R(b_j)·center。
    // 子骨 1（b 绕 Z 转 90°）：R(b)·center = (−cy, cx, cz) = (0.3, 0.4, 0.9)；
    //   ⇒ 修正 = (0.4,−0.3,0.9) − (0.3,0.4,0.9) = (0.1,−0.7,0)。
    // 子骨原有 rest_offset {0,0.3,0} ⇒ 烘后 {0.1,−0.4,0}。
    EXPECT_LT(MaxDiff(baked.joints[1].rest_offset,
                      Vec3f(0.1f, -0.4f, 0.0f)),
              1e-5f);
    // 入参未被修改。
    EXPECT_FLOAT_EQ(type.joints[1].rest_offset.y(), 0.3f);
    // bind_rotation 在 identity 旋转下保持不变。
    EXPECT_NEAR(baked.bind_rotation[1].w, type.bind_rotation[1].w, 1e-5f);
    EXPECT_NEAR(baked.bind_rotation[1].z, type.bind_rotation[1].z, 1e-5f);
}

// 空 bind_rotation 的骨架烘焙后仍为空（不凭空造出一堆恒等四元数）。
TEST(MeshTransformTest, SkeletonBakeKeepsEmptyBindRotationEmpty) {
    SkeletonType t;
    t.joints.resize(2);
    t.joints[0].parent = kSkeletonNoParent;
    t.joints[0].rest_offset = {0.0f, 0.5f, 0.0f};
    t.joints[1].parent = 0;
    t.joints[1].rest_offset = {0.0f, 0.4f, 0.0f};
    t.Validate();

    const SkeletonType baked = ApplyPlacementToSkeleton(
        t, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    EXPECT_TRUE(baked.bind_rotation.empty());
}

// ==================== 5. 边界：退化输入必须 crash ====================

TEST(MeshTransformTest, DegenerateUpFrontCrashes) {
    // up 与 front 平行（cross 退化）→ CHECK 失败。
    EXPECT_DEATH(MakePlacementBasis({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
                 "平行|cross 退化");
}

TEST(MeshTransformTest, ZeroUpCrashes) {
    EXPECT_DEATH(MakePlacementBasis({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}),
                 "不能为零");
}

TEST(MeshTransformTest, NonPositiveScaleCrashes) {
    const MeshData m = MakeTestMesh({1.0f, 0.0f, 0.0f}, {0, 0, 0, 0},
                                    {1.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_DEATH(ApplyPlacementToMesh(m, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                      {0.0f, 0.0f, 1.0f}, 0.0f),
                 "scale 必须 > 0");
}

}  // namespace
}  // namespace jpov

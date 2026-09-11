// Mixamo23Skeleton / SkeletonPose::Identity / SkeletonType::ComputeInverseBind 单元测试
//
// 覆盖：
//   1. 骨架结构：23 骨、拓扑序合法、骨名、Root 包装层
//   2. 骨长比例：官方 1.600m 骨架的关键骨长（Hips 高、腿长、臂长）与实测一致
//   3. height 缩放：等比缩放所有 rest_offset；bind_rotation 不受影响（旋转与尺度无关）
//   4. T-pose 自证：identity pose 下复现官方 T-pose 世界坐标
//      （头顶 ≈1.60m、手 ∓0.713m 水平、脚踩地、腿左右分开）—— 这是本工厂最硬的断言
//   5. ComputeInverseBind：尺寸正确、纯恒等逆的自洽性、位置还原（IBM 把 T-pose 世界点送回骨系）
//   6. SkeletonPose::Identity：全恒等 + size 对齐；bone_count<=0 崩溃
//   7. height 非法值崩溃

#include "tools/jpov/interface/mixamo23_skeleton.h"

#include <cmath>

#include "geom/common/math_util.h"
#include "geom/math/mat4.h"
#include "gtest/gtest.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {
namespace {

using geom::math::Mat4;
using geom::math::Mat4Mul;
using geom::math::Mat4TransformPoint;

// 把 SkeletonType + pose 沿树复合出每关节的 world（相对骨架空间原点）。
std::vector<Mat4> ComputeJointWorld(const SkeletonType& type,
                                    const SkeletonPose& pose) {
    const int n = type.bone_count();
    std::vector<Mat4> jw(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        const auto& bind = type.bind_rotation[static_cast<size_t>(j)];
        const auto& pr = pose.joint_rotation[static_cast<size_t>(j)];
        const Mat4 local = geom::math::JointLocal(
            type.joints[static_cast<size_t>(j)].rest_offset, bind, pr);
        const int p = type.joints[static_cast<size_t>(j)].parent;
        jw[static_cast<size_t>(j)] =
            (p == kSkeletonNoParent)
                ? local
                : Mat4Mul(jw[static_cast<size_t>(p)], local);
    }
    return jw;
}

// 骨名 → index（本表固定，测试里按名取更抗顺序变化）。
int IndexOf(const SkeletonType& type, const std::string& name) {
    for (int i = 0; i < type.bone_count(); ++i) {
        if (type.joints[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

// ── 1. 骨架结构 ──

TEST(Mixamo23SkeletonTest, Has23Bones) {
    const SkeletonType t = Mixamo23Skeleton();
    EXPECT_EQ(t.bone_count(), 23);
    EXPECT_EQ(t.bind_rotation.size(), 23u);
}

TEST(Mixamo23SkeletonTest, RootIsBoneZeroAndWrapper) {
    const SkeletonType t = Mixamo23Skeleton();
    // 0 号是 Root（无名），且无父（包装层/根）。
    EXPECT_EQ(t.joints[0].parent, kSkeletonNoParent);
    EXPECT_TRUE(t.joints[0].name.empty());
    // Hips 的父是 Root。
    EXPECT_EQ(t.joints[static_cast<size_t>(IndexOf(t, "mixamorig:Hips"))].parent, 0);
}

TEST(Mixamo23SkeletonTest, TopologyOrderIsValid) {
    const SkeletonType t = Mixamo23Skeleton();
    for (int i = 0; i < t.bone_count(); ++i) {
        const int p = t.joints[static_cast<size_t>(i)].parent;
        if (p != kSkeletonNoParent) {
            EXPECT_LT(p, i) << "joint " << i << " 的 parent 不满足拓扑序";
        }
    }
}

TEST(Mixamo23SkeletonTest, KeyBoneNamesPresent) {
    const SkeletonType t = Mixamo23Skeleton();
    for (const char* n : {"mixamorig:Hips", "mixamorig:Spine2", "mixamorig:Head",
                          "mixamorig:LeftArm", "mixamorig:RightHand",
                          "mixamorig:LeftUpLeg", "mixamorig:RightToeBase"}) {
        EXPECT_GE(IndexOf(t, n), 0) << "缺骨: " << n;
    }
}

TEST(Mixamo23SkeletonTest, BoneNamesUseMixamorigPrefix) {
    // 除 Root 外，全部用 mixamorig: 前缀（便于与 FBX 动画源按名对位）。
    const SkeletonType t = Mixamo23Skeleton();
    for (int i = 1; i < t.bone_count(); ++i) {
        EXPECT_EQ(t.joints[static_cast<size_t>(i)].name.rfind("mixamorig:", 0), 0u)
            << "骨 " << i << " 名不是 mixamorig: 前缀";
    }
}

// ── 2. 骨长比例（对官方 1.600m 骨架）──

TEST(Mixamo23SkeletonTest, OfficialBoneLengthsAtDefaultHeight) {
    // height = kMixamoOfficialBoneHeight → 缩放系数为 1，应精确复现官方骨长。
    const SkeletonType t = Mixamo23Skeleton(kMixamoOfficialBoneHeight);
    auto len = [&](const char* n) {
        return t.joints[static_cast<size_t>(IndexOf(t, n))].rest_offset.y();
    };
    EXPECT_NEAR(len("mixamorig:Spine"), 0.101824f, 1e-5f);
    EXPECT_NEAR(len("mixamorig:LeftForeArm"), 0.278415f, 1e-5f);
    EXPECT_NEAR(len("mixamorig:LeftLeg"), 0.443714f, 1e-5f);
    EXPECT_NEAR(len("mixamorig:LeftFoot"), 0.445278f, 1e-5f);
    // Hips 的骨长落在 +Y（官方布局：骨长轴 +Y）。
    const auto& hips = t.joints[static_cast<size_t>(IndexOf(t, "mixamorig:Hips"))];
    EXPECT_NEAR(hips.rest_offset.x(), 0.0f, 1e-6f);
    EXPECT_NEAR(hips.rest_offset.y(), 1.042749f, 1e-5f);
    EXPECT_NEAR(hips.rest_offset.z(), 0.0f, 1e-6f);
}

TEST(Mixamo23SkeletonTest, LeftRightAreMirroredOnX) {
    const SkeletonType t = Mixamo23Skeleton();
    auto off = [&](const char* n) {
        return t.joints[static_cast<size_t>(IndexOf(t, n))].rest_offset;
    };
    // 肩/胯的 x 分量左右反号（镜像）。
    EXPECT_NEAR(off("mixamorig:LeftShoulder").x(), -off("mixamorig:RightShoulder").x(), 1e-5f);
    EXPECT_NEAR(off("mixamorig:LeftUpLeg").x(), -off("mixamorig:RightUpLeg").x(), 1e-5f);
}

// ── 3. height 缩放 ──

TEST(Mixamo23SkeletonTest, HeightScalesRestOffsetProportionally) {
    const SkeletonType base = Mixamo23Skeleton(kMixamoOfficialBoneHeight);
    const SkeletonType tall = Mixamo23Skeleton(2.0f * kMixamoOfficialBoneHeight);
    const float k = 2.0f;
    for (int i = 0; i < base.bone_count(); ++i) {
        EXPECT_NEAR(tall.joints[static_cast<size_t>(i)].rest_offset.x(),
                    base.joints[static_cast<size_t>(i)].rest_offset.x() * k, 1e-5f);
        EXPECT_NEAR(tall.joints[static_cast<size_t>(i)].rest_offset.y(),
                    base.joints[static_cast<size_t>(i)].rest_offset.y() * k, 1e-5f);
        EXPECT_NEAR(tall.joints[static_cast<size_t>(i)].rest_offset.z(),
                    base.joints[static_cast<size_t>(i)].rest_offset.z() * k, 1e-5f);
    }
}

TEST(Mixamo23SkeletonTest, HeightDoesNotAffectBindRotation) {
    // 旋转与尺度无关：不同 height 造出的骨架，bind_rotation 必须逐位一致。
    const SkeletonType a = Mixamo23Skeleton(1.5f);
    const SkeletonType b = Mixamo23Skeleton(2.1f);
    for (int i = 0; i < a.bone_count(); ++i) {
        EXPECT_FLOAT_EQ(a.bind_rotation[static_cast<size_t>(i)].x,
                        b.bind_rotation[static_cast<size_t>(i)].x);
        EXPECT_FLOAT_EQ(a.bind_rotation[static_cast<size_t>(i)].w,
                        b.bind_rotation[static_cast<size_t>(i)].w);
    }
}

TEST(Mixamo23SkeletonTest, RejectsNonPositiveHeight) {
    EXPECT_DEATH(Mixamo23Skeleton(0.0f), "");
    EXPECT_DEATH(Mixamo23Skeleton(-1.0f), "");
}

// ── 4. T-pose 自证（本工厂最硬的断言）──

TEST(Mixamo23SkeletonTest, IdentityPoseReproducesOfficialTPose) {
    // ⚠️ 本测试是**外部锚点**：期望值(1.5993 / 0.7133 / 0.0821)来自**官方 FBX 实测**，
    //   独立于本仓库任何代码路径 —— 故能抓住"表填错 / 复合顺序错"这类实现 bug。
    //   它与下面 ComputeInverseBindPreservesDistances（内部自洽）**互补**，缺一不可。
    //
    // height = 官方骨骼身高 → identity pose 应精确复现官方 T-pose 世界坐标。
    const SkeletonType t = Mixamo23Skeleton(kMixamoOfficialBoneHeight);
    const SkeletonPose pose = SkeletonPose::Identity(t.bone_count());
    const std::vector<Mat4> jw = ComputeJointWorld(t, pose);

    auto pos = [&](const char* n) {
        return Mat4TransformPoint(jw[static_cast<size_t>(IndexOf(t, n))],
                                  Vec3f(0.0f, 0.0f, 0.0f));
    };

    // 头顶 ≈ 1.599m（骨骼身高定义点）。
    EXPECT_NEAR(pos("mixamorig:Head").y(), 1.5993f, 2e-3f);
    // 手尖水平伸出 ≈ ±0.713m（T-pose 手臂水平）。
    EXPECT_NEAR(pos("mixamorig:LeftHand").x(), 0.7133f, 2e-3f);
    EXPECT_NEAR(pos("mixamorig:RightHand").x(), -0.7133f, 2e-3f);
    // 手与肩同高（水平）：y 差应很小（<1cm）。
    EXPECT_NEAR(pos("mixamorig:LeftHand").y(), pos("mixamorig:LeftArm").y(), 1e-2f);
    // 脚踩地：Toe 的 y ≈ 0。
    EXPECT_NEAR(pos("mixamorig:LeftToeBase").y(), 0.0f, 2e-3f);
    // 左右腿分开 ≈ ±0.082m。
    EXPECT_NEAR(pos("mixamorig:LeftUpLeg").x(), 0.0821f, 2e-3f);
    EXPECT_NEAR(pos("mixamorig:RightUpLeg").x(), -0.0821f, 2e-3f);
}

TEST(Mixamo23SkeletonTest, TPoseScalesWithHeight) {
    // 身高 ×2 → T-pose 各点坐标也 ×2（等比缩放）。
    const SkeletonType t = Mixamo23Skeleton(2.0f * kMixamoOfficialBoneHeight);
    const SkeletonPose pose = SkeletonPose::Identity(t.bone_count());
    const std::vector<Mat4> jw = ComputeJointWorld(t, pose);
    const Vec3f head = Mat4TransformPoint(
        jw[static_cast<size_t>(IndexOf(t, "mixamorig:Head"))], Vec3f(0, 0, 0));
    EXPECT_NEAR(head.y(), 2.0f * 1.5993f, 5e-3f);
}

// ── 5. ComputeInverseBind ──

TEST(Mixamo23SkeletonTest, ComputeInverseBindShapeAndNontrivial) {
    const SkeletonType t = Mixamo23Skeleton();
    const auto ibm = t.ComputeInverseBind();
    EXPECT_EQ(ibm.size(), 23u);
    // 非平凡检查：全 0 / NaN 矩阵也能过 size 检查，必须另查内容。
    for (size_t j = 0; j < ibm.size(); ++j) {
        for (int k = 0; k < 16; ++k) {
            EXPECT_FALSE(std::isnan(ibm[j][k])) << "joint " << j << " 含 NaN";
        }
        // 末行须为 [0,0,0,1]（仿射），否则求逆/蒙皮会错。
        EXPECT_NEAR(ibm[j][3], 0.0f, 1e-5f);
        EXPECT_NEAR(ibm[j][7], 0.0f, 1e-5f);
        EXPECT_NEAR(ibm[j][11], 0.0f, 1e-5f);
        EXPECT_NEAR(ibm[j][15], 1.0f, 1e-5f);
        // 平移列不能全 0（root 的 IBM 平移分量实际为 0，故只要求"不全是 0 矩阵"）。
    }
    // 至少有一个关节的 IBM 非单位阵（Hips 有 1.04m 平移 → 其逆必含平移）。
    const int hips = IndexOf(t, "mixamorig:Hips");
    ASSERT_GE(hips, 0);
    const auto& h = ibm[static_cast<size_t>(hips)];
    const float trans_norm = std::sqrt(h[12] * h[12] + h[13] * h[13] + h[14] * h[14]);
    EXPECT_GT(trans_norm, 0.5f) << "Hips 的 IBM 平移应非零（骨长 1.04m）";
}

TEST(Mixamo23SkeletonTest, ComputeInverseBindPreservesDistances) {
    // **刚性不变量**门禁：IBM 是刚性逆变换（旋转+平移），故**相对该关节自身原点的距离**守恒：
    //   |IBM·p - IBM·(j 的世界原点)| == |p - (j 的世界原点)|
    // （注意：不是"到世界原点的距离"——jw 含平移，非正交阵，那种比较不成立。这是一个实际踩过的坑：
    //   初版写成 |IBM·p| vs |jw·p| 直接失败（比值 ≈ 4.3），属于**测试前提写错**而非代码错。）
    //
    // 这个断言独立于"jw 用哪个公式算"，只要 IBM 是刚性逆就必须成立，故能抓住
    // "IBM 少乘/多乘一项导致缩放错"这类 bug（乘积=I 那种恒真式抓不到）。
    const SkeletonType t = Mixamo23Skeleton(kMixamoOfficialBoneHeight);
    const SkeletonPose pose = SkeletonPose::Identity(t.bone_count());
    const std::vector<Mat4> jw = ComputeJointWorld(t, pose);
    const auto ibm = t.ComputeInverseBind();

    // 一个不在任何关节上的测试点。
    const Vec3f probe(0.31f, 0.77f, -0.12f);
    for (int j = 0; j < t.bone_count(); ++j) {
        Mat4 m;
        for (int k = 0; k < 16; ++k) m.m[k] = ibm[static_cast<size_t>(j)][k];
        const Mat4 jwm = jw[static_cast<size_t>(j)];

        // 关节 j 的世界原点（jw 的平移列），及其在骨系下的像（应为原点）。
        const Vec3f world_origin = Mat4TransformPoint(jwm, Vec3f(0, 0, 0));
        const Vec3f bone_origin = Mat4TransformPoint(m, world_origin);

        const Vec3f in_bone = Mat4TransformPoint(m, probe);
        // 探针相对关节原点的偏移（两边同一参照）。
        const float d_bone = std::sqrt(geom::Sqr(in_bone.x() - bone_origin.x()) +
                                       geom::Sqr(in_bone.y() - bone_origin.y()) +
                                       geom::Sqr(in_bone.z() - bone_origin.z()));
        const float d_world = std::sqrt(geom::Sqr(probe.x() - world_origin.x()) +
                                        geom::Sqr(probe.y() - world_origin.y()) +
                                        geom::Sqr(probe.z() - world_origin.z()));
        EXPECT_NEAR(d_bone, d_world, 1e-3f) << "joint " << j << " 相对原点距离不守恒（IBM 非刚性）";
        // 且关节自身世界原点必须落到骨系原点（IBM 的定义性质）。
        EXPECT_NEAR(bone_origin.x(), 0.0f, 1e-4f) << "joint " << j;
        EXPECT_NEAR(bone_origin.y(), 0.0f, 1e-4f) << "joint " << j;
        EXPECT_NEAR(bone_origin.z(), 0.0f, 1e-4f) << "joint " << j;
    }
}

TEST(Mixamo23SkeletonTest, DistanceGateWouldCatchAWrongInverse) {
    // 负向验证：故意给一个错的 inverse（强行缩放 1.5 倍），门禁必须能 FAIL。
    // 这直接证明上一条门禁**真的能区分对错**，不是恒真断言。
    const SkeletonType t = Mixamo23Skeleton(kMixamoOfficialBoneHeight);
    const SkeletonPose pose = SkeletonPose::Identity(t.bone_count());
    const std::vector<Mat4> jw = ComputeJointWorld(t, pose);
    auto ibm = t.ComputeInverseBind();

    // 把 0 号关节的 IBM 强行缩放 1.5 倍（错的值）。
    for (int k = 0; k < 12; ++k) ibm[0][k] *= 1.5f;

    const Vec3f probe(0.31f, 0.77f, -0.12f);
    Mat4 m;
    for (int k = 0; k < 16; ++k) m.m[k] = ibm[0][k];
    const Vec3f world_origin = Mat4TransformPoint(jw[0], Vec3f(0, 0, 0));
    const Vec3f bone_origin = Mat4TransformPoint(m, world_origin);
    const Vec3f in_bone = Mat4TransformPoint(m, probe);
    const float d_bone = std::sqrt(geom::Sqr(in_bone.x() - bone_origin.x()) +
                                   geom::Sqr(in_bone.y() - bone_origin.y()) +
                                   geom::Sqr(in_bone.z() - bone_origin.z()));
    const float d_world = std::sqrt(geom::Sqr(probe.x() - world_origin.x()) +
                                    geom::Sqr(probe.y() - world_origin.y()) +
                                    geom::Sqr(probe.z() - world_origin.z()));
    // 错的 IBM 必须让距离不守恒（差值远超上一条门禁的 1e-3 容差）。
    EXPECT_GT(std::fabs(d_bone - d_world), 1e-2f)
        << "距离门禁对错误的 IBM 未能报错 → 上一条断言是恒真的";
}

// ── 6. SkeletonPose::Identity ──

TEST(SkeletonPoseIdentityTest, IsAllIdentity) {
    const SkeletonPose p = SkeletonPose::Identity(23);
    EXPECT_EQ(p.bone_count, 23);
    ASSERT_EQ(p.joint_rotation.size(), 23u);
    for (const auto& q : p.joint_rotation) {
        EXPECT_FLOAT_EQ(q.x, 0.0f);
        EXPECT_FLOAT_EQ(q.y, 0.0f);
        EXPECT_FLOAT_EQ(q.z, 0.0f);
        EXPECT_FLOAT_EQ(q.w, 1.0f);
    }
    EXPECT_FLOAT_EQ(p.root_offset.x(), 0.0f);
    EXPECT_FLOAT_EQ(p.root_offset.y(), 0.0f);
    EXPECT_FLOAT_EQ(p.root_offset.z(), 0.0f);
}

TEST(SkeletonPoseIdentityTest, RejectsNonPositiveBoneCount) {
    EXPECT_DEATH(SkeletonPose::Identity(0), "");
    EXPECT_DEATH(SkeletonPose::Identity(-3), "");
}

TEST(SkeletonPoseIdentityTest, SizeMatchesSkeletonSoBakeAcceptsIt) {
    const SkeletonType t = Mixamo23Skeleton();
    const SkeletonPose p = SkeletonPose::Identity(t.bone_count());
    EXPECT_EQ(p.joint_rotation.size(), static_cast<size_t>(t.bone_count()));
}

}  // namespace
}  // namespace jpov

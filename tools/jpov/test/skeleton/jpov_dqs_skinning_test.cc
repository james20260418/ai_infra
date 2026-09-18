// jpov_dqs_skinning_test — DQS 蒙皮物理性质测试（**纯 CPU，不需要 GL/DISPLAY**）
//
// 为什么要这条（配合 test/skeleton/jpov_skinned_multipose_test）：
//   multipose 测试证明的是「GPU 的 DQS 与 CPU 真值逐像素同形」（实现正确性）；
//   本测试证明的是「**为什么**要换 DQS」（行为正确性）—— 即
//     「关节弯折时 LBS 把顶点拉向弦（体积塌缩），DQS 不塌」这一条，用可判定的数字钉住。
//
// 测法（最小可控骨架 + 手写权重，不依赖任何资产）：
//   造一根 2 骨的「手臂」骨架，第 2 根骨相对第 1 根转 90°（典型肘部弯折），
//   再取两个**权重完全相同**的顶点（都在 50/50 混合区）：
//     · 权重相同 ⇒ 两者拿到的是**同一个**混合变换 ⇒
//         DQS：该变换是刚体 ⇒ 两点距离**保持不变**（1e-5 内）；
//         LBS：该变换是矩阵加权平均（非正交）⇒ 距离被压到 cos45° ≈ 0.707 倍。
//   这就是「关节弯折处体积塌缩」的最小复现，也是 DQS 的收益量化。
//
// 复用真实管线的那部分：SkinMatricesOnCpuForTest（沿树解算 + 折 inverseBind）——
//   即测试跑的就是渲染用的同一条链路的前半段；后半段（DQS/LBS 混合）分别走
//   SkinMeshOnCpuForTest / SkinMeshOnCpuForTestLinearBlend（与 shader 同公式）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "gtest/gtest.h"

#include "geom/math/dual_quat.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/skeleton/jpov_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ── 最小 2 骨骨架：root 在原点，child 沿 +Y 伸出 0.1（长度 0.1 米）。
//    bind_rotation 故意留空（= 全恒等）⇒ 骨长即朝向，最容易手推：joint 位在 (0,0.1,0)。
struct TwoBoneRig {
    jpov::SkeletonType type;
    jpov::SkeletonPose pose_bend;    // child 相对父绕 Z 转 90°
    jpov::SkeletonPose pose_rest;    // 全恒等
};

TwoBoneRig MakeTwoBoneRig() {
    TwoBoneRig rig;
    rig.type.joints.resize(2);
    rig.type.joints[0].parent = jpov::kSkeletonNoParent;
    rig.type.joints[0].rest_offset = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    rig.type.joints[0].name = "root";
    rig.type.joints[1].parent = 0;
    rig.type.joints[1].rest_offset = jpov::Vec3f(0.0f, 0.1f, 0.0f);
    rig.type.joints[1].name = "child";
    // bind_rotation 留空（identity）——注释在 skeleton_types.h：空 = 骨长即朝向。
    rig.type.Validate();

    const int bone = rig.type.bone_count();
    rig.pose_rest = jpov::SkeletonPose::Identity(bone);
    rig.pose_bend = jpov::SkeletonPose::Identity(bone);
    rig.pose_bend.joint_rotation[1] = geom::Quaternion<float>::FromAxisAngle(
        jpov::Vec3f(0.0f, 0.0f, 1.0f), 90.0f * kPi / 180.0f);
    return rig;
}

// 造一个「两顶点权重完全相同」的 rest mesh：两点只在 Y 上相差 d（都在 50/50 混合区）。
//   其余字段（indices/uvs/normals）非必需 —— Validate 只查长度对齐；这里显式给 normals
//   以便顺带确认「法线也走同一混合」不崩。
jpov::MeshData MakeEqualWeightPairMesh(const jpov::Vec3f& a, const jpov::Vec3f& b,
                                       float w_root) {
    jpov::MeshData m;
    // 位掩码组合：本工程未给 MeshVertexFlags 重载 operator|，用显式转换拼（同 gltf_loader.cc）。
    // 不声明 kTangent（Validate 要求切线需 kNormal + kUV，本例无需 UV）。
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kJoints));
    m.positions = {a, b};
    m.normals = {jpov::Vec3f(0.0f, 1.0f, 0.0f), jpov::Vec3f(0.0f, 1.0f, 0.0f)};
    const float w_child = 1.0f - w_root;
    m.joint_indices = {std::array<int32_t, 4>{0, 1, 0, 0}, std::array<int32_t, 4>{0, 1, 0, 0}};
    m.joint_weights = {std::array<float, 4>{w_root, w_child, 0.0f, 0.0f},
                       std::array<float, 4>{w_root, w_child, 0.0f, 0.0f}};
    m.Validate();
    return m;
}

float Dist(const jpov::Vec3f& a, const jpov::Vec3f& b) {
    const jpov::Vec3f d = a - b;
    return std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
}

}  // namespace

// ── 1. rest/identity pose：两种混合都应还原 rest（都不动）──

TEST(DqsSkinningTest, RestPoseLeavesVerticesInPlaceForBothBlends) {
    const TwoBoneRig rig = MakeTwoBoneRig();
    const jpov::MeshData rest =
        MakeEqualWeightPairMesh(jpov::Vec3f(0.0f, 0.05f, 0.0f), jpov::Vec3f(0.0f, 0.07f, 0.0f),
                                /*w_root=*/0.5f);
    const jpov::MeshData dqs = jpov_skeleton_gold::SkinMeshOnCpuForTest(
        rig.type, rig.pose_rest, rest);
    const jpov::MeshData lbs = jpov_skeleton_gold::SkinMeshOnCpuForTestLinearBlend(
        rig.type, rig.pose_rest, rest);
    for (size_t i = 0; i < rest.positions.size(); ++i) {
        EXPECT_NEAR(Dist(dqs.positions[i], rest.positions[i]), 0.0f, 1e-5f)
            << "DQS 在 rest pose 下不该动顶点 i=" << i;
        EXPECT_NEAR(Dist(lbs.positions[i], rest.positions[i]), 0.0f, 1e-5f)
            << "LBS 在 rest pose 下不该动顶点 i=" << i;
    }
}

// ── 2. 核心：90° 弯折处，DQS 保距（保体积），LBS 塌陷 ──

TEST(DqsSkinningTest, BentJointKeepsDistanceWhileLbsCollapses) {
    const TwoBoneRig rig = MakeTwoBoneRig();
    // 两点都在 50/50 混合区（权重相同），沿 Y 相距 0.02。
    const jpov::Vec3f a(0.0f, 0.05f, 0.0f);
    const jpov::Vec3f b(0.0f, 0.07f, 0.0f);
    const jpov::MeshData rest = MakeEqualWeightPairMesh(a, b, /*w_root=*/0.5f);
    const float d0 = Dist(a, b);

    const jpov::MeshData dqs = jpov_skeleton_gold::SkinMeshOnCpuForTest(
        rig.type, rig.pose_bend, rest);
    const jpov::MeshData lbs = jpov_skeleton_gold::SkinMeshOnCpuForTestLinearBlend(
        rig.type, rig.pose_bend, rest);

    const float d_dqs = Dist(dqs.positions[0], dqs.positions[1]);
    const float d_lbs = Dist(lbs.positions[0], lbs.positions[1]);
    LOG(INFO) << "弯折 90°：两点距离 原始=" << d0 << " DQS=" << d_dqs << " LBS=" << d_lbs
              << "（LBS/原始=" << (d_lbs / d0) << "，理论 cos45°=0.7071）";

    EXPECT_NEAR(d_dqs, d0, 1e-5f) << "DQS 混合是刚体变换 ⇒ 必须保距（不塌体积）";
    // LBS 的矩阵平均把长度压到 cos(45°) 量级 —— 这条同时是“测试真的在测东西”的证据。
    EXPECT_NEAR(d_lbs / d0, 0.70710678f, 0.02f)
        << "LBS 对照应塌到 cos45° 附近（若没塌，说明这条门禁测不到东西）";
    EXPECT_LT(d_dqs, d0 * 1.0001f);
    EXPECT_GT(d_dqs, d_lbs * 1.3f) << "DQS 必须明显优于 LBS（差值太小 ⇒ 混合方式实际没换）";
}

// ── 3. 双帧插值同样保距（旧实现是矩阵 lerp，中间帧照样塌）──

TEST(DqsSkinningTest, TwoFrameInterpolationStaysRigid) {
    const TwoBoneRig rig = MakeTwoBoneRig();
    const jpov::Vec3f a(0.0f, 0.05f, 0.0f);
    const jpov::Vec3f b(0.0f, 0.07f, 0.0f);
    const jpov::MeshData rest = MakeEqualWeightPairMesh(a, b, /*w_root=*/0.5f);
    const float d0 = Dist(a, b);

    for (float ratio : {0.25f, 0.5f, 0.75f}) {
        const jpov::MeshData dqs = jpov_skeleton_gold::SkinMeshOnCpuForTest(
            rig.type, rig.pose_rest, rig.pose_bend, ratio, rest);
        const jpov::MeshData lbs = jpov_skeleton_gold::SkinMeshOnCpuForTestLinearBlend(
            rig.type, rig.pose_rest, rig.pose_bend, ratio, rest);
        const float d_dqs = Dist(dqs.positions[0], dqs.positions[1]);
        const float d_lbs = Dist(lbs.positions[0], lbs.positions[1]);
        LOG(INFO) << "插值 ratio=" << ratio << "：DQS 距离=" << d_dqs << " LBS 距离=" << d_lbs;
        EXPECT_NEAR(d_dqs, d0, 1e-5f) << "ratio=" << ratio << " 的插值帧也必须保距";
        EXPECT_LT(d_lbs, d0 * 0.98f)
            << "ratio=" << ratio << " 的 LBS 插值帧应塌陷（对照；否则门禁无效）";
    }
}

// ── 5. 真实资产（mixamo_male.glb）：全局边长比统计 —— LBS 系统性收缩，DQS 不收缩 ──
//
// 说明（别过度解读）：DQS 是**逐顶点刚体**（不同权重的顶点得到不同刚体变换），故边长整体
//   变化也不会为零；但刚体映射不带“把长度按夹角余弦压扁”这个**系统性**偏差 ⇒ 边长比中位数
//   应贴近 1。LBS 的矩阵平均则会让大量边**系统性变短**（关节处尤其）。
// 本用例跑的就是渲染用的那条链路（SkinMatricesOnCpuForTest + 两个混合实现）。
TEST(DqsSkinningTest, RealAssetEdgeLengthsStayUnshrunk) {
    const std::string glb =
        jpov::GetProjectRoot() + "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";
    jpov::MeshData rest;
    jpov::GltfMaterialInfo mi;
    ASSERT_TRUE(jpov::LoadGltf(glb, &rest, &mi)) << "读不到资产网格: " << glb;
    std::vector<jpov::SkeletonType> skels;
    ASSERT_TRUE(jpov::LoadGltfSkeleton(glb, &skels));
    ASSERT_FALSE(skels.empty());
    const jpov::SkeletonType& type = skels[0];
    ASSERT_EQ(rest.joint_indices.size(), rest.positions.size()) << "该网格应带 joints/weights";
    ASSERT_FALSE(rest.indices.empty());

    // 用与 multipose 测试同一批「极端」姿态（每帧叠加大角度旋转），使关节处真有大幅混合。
    const std::vector<jpov::SkeletonPose> poses = jpov_skeleton_gold::MakeMultiPoses(type.bone_count());
    ASSERT_GE(poses.size(), 2u);

    // 收集所有三角形边（去重不做，统计量对重复不敏感；只求中位数/分位数）。
    std::vector<std::array<size_t, 2>> edges;
    for (size_t t = 0; t + 2 < rest.indices.size(); t += 3) {
        const size_t i0 = rest.indices[t], i1 = rest.indices[t + 1], i2 = rest.indices[t + 2];
        edges.push_back({i0, i1});
        edges.push_back({i1, i2});
        edges.push_back({i2, i0});
    }
    ASSERT_FALSE(edges.empty());

    std::vector<float> ratio_dqs;
    std::vector<float> ratio_lbs;
    for (size_t p = 1; p < poses.size(); ++p) {
        const jpov::MeshData md = jpov_skeleton_gold::SkinMeshOnCpuForTest(type, poses[p], rest);
        const jpov::MeshData ml =
            jpov_skeleton_gold::SkinMeshOnCpuForTestLinearBlend(type, poses[p], rest);
        for (const std::array<size_t, 2>& e : edges) {
            const float d0 = Dist(rest.positions[e[0]], rest.positions[e[1]]);
            if (d0 < 1e-6f) {
                continue;  // 退化边（重合点）不参与统计
            }
            ratio_dqs.push_back(Dist(md.positions[e[0]], md.positions[e[1]]) / d0);
            ratio_lbs.push_back(Dist(ml.positions[e[0]], ml.positions[e[1]]) / d0);
        }
    }
    ASSERT_FALSE(ratio_dqs.empty());
    auto percentile = [](std::vector<float> v, double p) {
        std::sort(v.begin(), v.end());
        size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
        return v[i];
    };
    auto frac_below = [](const std::vector<float>& v, float thr) {
        size_t n = 0;
        for (float r : v) {
            if (r < thr) {
                ++n;
            }
        }
        return static_cast<double>(n) / static_cast<double>(v.size());
    };
    LOG(INFO) << "真实资产边长比（边数=" << ratio_dqs.size() << "）"
              << "：DQS p1=" << percentile(ratio_dqs, 0.01)
              << " p5=" << percentile(ratio_dqs, 0.05)
              << " 中位=" << percentile(ratio_dqs, 0.5)
              << " | LBS p1=" << percentile(ratio_lbs, 0.01)
              << " p5=" << percentile(ratio_lbs, 0.05)
              << " 中位=" << percentile(ratio_lbs, 0.5);
    LOG(INFO) << "明显缩短的边占比（< 0.98）：DQS=" << frac_below(ratio_dqs, 0.98f)
              << " LBS=" << frac_below(ratio_lbs, 0.98f) << "（< 0.95）：DQS="
              << frac_below(ratio_dqs, 0.95f) << " LBS=" << frac_below(ratio_lbs, 0.95f);

    // 门禁：在“确实发生混合”的那一小部分边上，LBS 的缩短必须明显多于 DQS。
    //   （中位数两边都是 1：绝大多数边完整落在单根骨内 —— 单骨顶点的变换在两种写法下都是
    //     同一个刚体，完全一样。所以这条只该拿尾部统计量说话，别拿中位数说事。）
    EXPECT_GT(frac_below(ratio_lbs, 0.95f), frac_below(ratio_dqs, 0.95f))
        << "LBS 的明显缩短边占比应高于 DQS（否则这条门禁测不到东西）";
    EXPECT_LT(percentile(ratio_dqs, 0.01), 0.999f)
        << "DQS 也不该是恒 1（否则说明混合根本没发生，测试无效）";
}


// ── 4. 权重不同的顶点：两者都会“动”，但 DQS 的每个顶点都还是刚体映射 ──
TEST(DqsSkinningTest, DifferentWeightsStayFiniteAndBounded) {
    const TwoBoneRig rig = MakeTwoBoneRig();
    // 两顶点**权重不同**（0.3 / 0.7）：DQS 下各自拿到不同的刚体变换 ⇒ 距离**可以**变，
    //   但每个顶点都不应跑到“骨架可达范围”之外（这里用 |v| 上界做粗门禁，抓 NaN/爆炸）。
    jpov::MeshData rest = MakeEqualWeightPairMesh(jpov::Vec3f(0.0f, 0.05f, 0.0f),
                                                  jpov::Vec3f(0.0f, 0.07f, 0.0f), 0.5f);
    rest.joint_weights[1] = std::array<float, 4>{0.3f, 0.7f, 0.0f, 0.0f};
    const jpov::MeshData dqs = jpov_skeleton_gold::SkinMeshOnCpuForTest(
        rig.type, rig.pose_bend, rest);
    for (size_t i = 0; i < dqs.positions.size(); ++i) {
        const float r = std::sqrt(dqs.positions[i].x() * dqs.positions[i].x() +
                                  dqs.positions[i].y() * dqs.positions[i].y() +
                                  dqs.positions[i].z() * dqs.positions[i].z());
        EXPECT_TRUE(std::isfinite(r)) << "顶点 " << i << " 出现了 NaN/Inf";
        EXPECT_LT(r, 0.5f) << "顶点 " << i << " 跑出了骨架尺度（0.1 米骨长）之外";
    }
}

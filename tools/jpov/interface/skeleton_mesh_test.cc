// JPOV skeleton mesh 生成单测（纯 CPU，不需 GL / Xvfb）
//
// 验证 BuildBoneMeshInBoneSpace + MeshData::MakeOrientedBox：
//   1. 结构：骨数=非零长骨数、顶点数=24×杆数、flags 含 kPosition|kNormal|kJoints、Validate() 通过
//   2. 蒙皮属性：每顶点 joint_indices 4 组全同且 ∈ [0,bone_count)，weights={1,0,0,0}
//   3. **几何自证（核心）**：每根杆的两个"端面中心"必须落在该骨在**骨架空间**的
//      父关节位置与子关节位置（= JW_bind 的平移）—— 这是真正能区分对错的断言，
//      而不是"size()==N"这类恒真检查。
//   4. utils：MakeOrientedBox 的旋转/正交化/退化保护。

#include <cmath>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "geom/math/mat4.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace {

using jpov::MeshData;
using jpov::MeshVertexFlags;
using jpov::SkeletonPose;
using jpov::SkeletonType;
using jpov::Vec3f;

// 规格：根杆半宽 = radius/3（2026-09-14 Danis 定）。
// ⚠️ 测试里硬编码该值、**不用 jpov::kRootRodRadiusScale** —— 否则实现常量一改、
// 期望值同变 → 断言恒真（本测试的负向验证抓过这个坑，勿重蹈）。
constexpr float kRootRodRadiusScaleSpec = 1.0f / 3.0f;

// 沿拓扑序算每关节在骨架空间下的 bind 变换（pose 恒等）。
std::vector<geom::math::Mat4> ComputeJointWorldBind(const SkeletonType& type) {
    const size_t n = type.joints.size();
    std::vector<geom::math::Mat4> jw(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> bind =
            type.bind_rotation.empty() ? geom::Quaternion<float>::Identity()
                                       : type.bind_rotation[j];
        const geom::math::Mat4 local =
            geom::math::JointLocalRest(type.joints[j].rest_offset, bind);
        const int p = type.joints[j].parent;
        jw[j] = (p == jpov::kSkeletonNoParent)
                    ? local
                    : geom::math::Mat4Mul(jw[static_cast<size_t>(p)], local);
    }
    return jw;
}

// 数非零长度的骨（= 会生成杆的骨；零长包装层如 Root 不画）。
int CountDrawableBones(const SkeletonType& type) {
    int cnt = 0;
    for (const auto& j : type.joints) {
        if (j.rest_offset.Norm() >= jpov::kDegenerateRodLength) {
            ++cnt;
        }
    }
    return cnt;
}

// ==================== 1. 结构与 Validate ====================

TEST(SkeletonMesh, StructureAndValidate) {
    const SkeletonType type = jpov::Mixamo23Skeleton(1.75f);
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, 0.04f);

    // Validate() 在函数内已调用；此处再显式调一次确保与 mesh.h 约束一致。
    mesh.Validate();

    const int drawable = CountDrawableBones(type);
    EXPECT_GT(drawable, 0);
    // 每根杆 = 6 面 × 4 顶点 = 24 顶点；每根杆 36 index。
    EXPECT_EQ(mesh.positions.size(), static_cast<size_t>(24 * drawable))
        << "顶点数应为 24×杆数";
    EXPECT_EQ(mesh.indices.size(), static_cast<size_t>(36 * drawable))
        << "索引数应为 36×杆数";
    ASSERT_EQ(mesh.normals.size(), mesh.positions.size());
    ASSERT_EQ(mesh.joint_indices.size(), mesh.positions.size());
    ASSERT_EQ(mesh.joint_weights.size(), mesh.positions.size());

    EXPECT_TRUE(jpov::MeshHasFlag(mesh.flags, MeshVertexFlags::kPosition));
    EXPECT_TRUE(jpov::MeshHasFlag(mesh.flags, MeshVertexFlags::kNormal));
    EXPECT_TRUE(jpov::MeshHasFlag(mesh.flags, MeshVertexFlags::kJoints));
}

// ==================== 2. 蒙皮属性 ====================

TEST(SkeletonMesh, SkinAttributes) {
    const SkeletonType type = jpov::Mixamo23Skeleton(1.75f);
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, 0.04f);

    std::set<int32_t> used_joints;
    for (size_t v = 0; v < mesh.positions.size(); ++v) {
        const std::array<int32_t, 4>& ji = mesh.joint_indices[v];
        // 4 组 joint 全同（单骨影响），且为合法骨架索引。
        EXPECT_EQ(ji[0], ji[1]);
        EXPECT_EQ(ji[0], ji[2]);
        EXPECT_EQ(ji[0], ji[3]);
        EXPECT_GE(ji[0], 0);
        EXPECT_LT(ji[0], type.bone_count());
        used_joints.insert(ji[0]);

        // 权重全给第一个 joint（刚性绑定）。
        const std::array<float, 4>& w = mesh.joint_weights[v];
        EXPECT_FLOAT_EQ(w[0], 1.0f);
        EXPECT_FLOAT_EQ(w[1], 0.0f);
        EXPECT_FLOAT_EQ(w[2], 0.0f);
        EXPECT_FLOAT_EQ(w[3], 0.0f);
    }

    // 每根可画骨都应有对应杆 → 用到的 joint 数 == 可画骨数。
    EXPECT_EQ(static_cast<int>(used_joints.size()), CountDrawableBones(type));
    // 零长根（Root）不应被任何顶点引用。
    EXPECT_EQ(used_joints.count(0), 0u) << "零长 Root 不应产生杆";
}

// ==================== 3. 几何自证（核心）====================

// 每根杆沿骨长轴的两个端面中心，必须等于该骨父/子关节的骨架空间坐标（JW_bind 平移）。
TEST(SkeletonMesh, RodEndpointsMatchJointPositions) {
    const SkeletonType type = jpov::Mixamo23Skeleton(1.75f);
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, 0.04f);

    const std::vector<geom::math::Mat4> jw = ComputeJointWorldBind(type);
    constexpr float kRadius = 0.04f;   // 与 BuildBoneMeshInBoneSpace 传入值一致

    // 杆是逐骨 append 的，顶点按骨顺序排列：第 k 根杆占 [24k, 24k+24)。
    // 逐骨遍历（跳过零长骨），核对其端点。
    size_t rod_index = 0;
    for (size_t j = 0; j < type.joints.size(); ++j) {
        const Vec3f& off = type.joints[j].rest_offset;
        if (off.Norm() < jpov::kDegenerateRodLength) {
            continue;  // 零长骨无杆
        }

        const size_t base = rod_index * 24;
        // 该杆 24 顶点中任取前 24 个算 AABB —— 端点即 AABB 沿长轴的两端。
        // 用两面中心的平均更稳：直接取 24 顶点的质心 = 杆中心。
        Vec3f centroid(0.0f, 0.0f, 0.0f);
        for (size_t k = 0; k < 24; ++k) {
            centroid += mesh.positions[base + k];
        }
        centroid = Vec3f(centroid.x() / 24.0f, centroid.y() / 24.0f,
                         centroid.z() / 24.0f);

        // 期望杆中心 = 父关节与子关节位置的中点（骨架空间）。
        const int p = type.joints[j].parent;
        const Vec3f child_pos = geom::math::Mat4TranslationOf(jw[j]);
        const Vec3f parent_pos =
            (p == jpov::kSkeletonNoParent)
                ? Vec3f(0.0f, 0.0f, 0.0f)  // 根：父系 = 骨架空间原点
                : geom::math::Mat4TranslationOf(jw[static_cast<size_t>(p)]);
        const Vec3f expect_center((parent_pos.x() + child_pos.x()) * 0.5f,
                                  (parent_pos.y() + child_pos.y()) * 0.5f,
                                  (parent_pos.z() + child_pos.z()) * 0.5f);

        EXPECT_NEAR(centroid.x(), expect_center.x(), 1e-4f)
            << "骨[" << j << "] " << type.joints[j].name << " 杆中心 x 不符";
        EXPECT_NEAR(centroid.y(), expect_center.y(), 1e-4f)
            << "骨[" << j << "] " << type.joints[j].name << " 杆中心 y 不符";
        EXPECT_NEAR(centroid.z(), expect_center.z(), 1e-4f)
            << "骨[" << j << "] " << type.joints[j].name << " 杆中心 z 不符";

        // 骨轴方向（外部参照，来自骨架数据而非 mesh 自身）：parent→child。
        const Vec3f axis = Vec3f(child_pos.x() - parent_pos.x(),
                                 child_pos.y() - parent_pos.y(),
                                 child_pos.z() - parent_pos.z())
                               .Unit();

        // 杆的**朝向**才是关键：所有顶点到杆中心的偏移，其在骨轴上的垂直分量
        // 必须 <= sqrt(2)*radius（角点垂直偏移最大 = sqrt(r²+r²)）。
        // 若杆轴向装错（骨轴与 parent→child 方向不平行），原本沿轴 ±|off|/2 的端面顶点
        // 会产生 sin(θ)·|off|/2 的垂直泄漏 → 超出 sqrt(2)·radius → 被抓住。
        // （注意：只查质心是**测不出朝向错误**的——box 顶点关于自身中心对称，任何旋转
        //  下质心都等于变换后的中心。必须查各顶点的垂直分量才能锁住朝向。）
        const float perp_limit = std::sqrt(2.0f) * kRadius + 1e-4f;
        for (size_t k = 0; k < 24; ++k) {
            const Vec3f d(mesh.positions[base + k].x() - centroid.x(),
                          mesh.positions[base + k].y() - centroid.y(),
                          mesh.positions[base + k].z() - centroid.z());
            const float along = d.x() * axis.x() + d.y() * axis.y() + d.z() * axis.z();
            const Vec3f perp(d.x() - along * axis.x(),
                             d.y() - along * axis.y(),
                             d.z() - along * axis.z());
            EXPECT_LE(perp.Norm(), perp_limit)
                << "骨[" << j << "] " << type.joints[j].name
                << " 顶点偏离骨轴过多 —— 杆朝向错误";
        }

        // 杆沿骨轴方向的半长应 ≈ |off|/2（锁住“半长传错”这一类错误）。
        // 注：box 顶点全在两端面上，故“最大轴向投影”对正确实现恒 = |off|/2；
        //     它不能抓朝向错误（那是上面垂直分量断言的职责），但能抓半长被传错。
        float max_along = 0.0f;
        for (size_t k = 0; k < 24; ++k) {
            const Vec3f d(mesh.positions[base + k].x() - centroid.x(),
                          mesh.positions[base + k].y() - centroid.y(),
                          mesh.positions[base + k].z() - centroid.z());
            const float along = std::fabs(d.x() * axis.x() + d.y() * axis.y() +
                                          d.z() * axis.z());
            max_along = std::max(max_along, along);
        }
        EXPECT_NEAR(max_along, off.Norm() * 0.5f, 1e-4f)
            << "骨[" << j << "] " << type.joints[j].name << " 杆沿轴半长应=|rest_offset|/2";

        ++rod_index;
    }
    EXPECT_EQ(rod_index, static_cast<size_t>(CountDrawableBones(type)));
}

// 专项锁死杆**朝向**：造一个非轴对齐且与 +Y 夹角很大的骨，
// 确保“用错骨轴方向”这类错误被稳定抓住（不依赖 Mixamo 恰好有斜骨）。
TEST(SkeletonMesh, RodAxisFollowsRestOffsetDirection) {
    // 单骨，rest_offset 方向 = (1,1,0)/√2 —— 与 +Y 成 45°，与 +Z 垂直。
    // 若实现误用局部 +Y 作骨轴，端面顶点会大量偏离 (1,1,0) 方向 → 必被抓住。
    SkeletonType type;
    type.joints.resize(1);
    type.joints[0].parent = jpov::kSkeletonNoParent;
    type.joints[0].rest_offset = Vec3f(0.2f, 0.2f, 0.0f);
    type.joints[0].name = "slanted";
    type.Validate();

    constexpr float kR = 0.03f;
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, kR);

    // 期望杆中心 = rest_offset/2，骨轴 = normalize(rest_offset)。
    const Vec3f axis =
        Vec3f(type.joints[0].rest_offset.x(), type.joints[0].rest_offset.y(),
              type.joints[0].rest_offset.z())
            .Unit();
    const Vec3f center(0.1f, 0.1f, 0.0f);

    // 1) 每个顶点到中心的偏移，垂直分量 <= √2·半宽。
    //    ⚠️ 本骨是根关节（parent=无）→ 杆半宽收窄为 kR/3（规格值，见 kRootRodRadiusScaleSpec）；
    //    上限取 √2·(kR/3)。方向装错时垂直泄漏 ≈ 0.1 量级，远大于该上限 → 必被抓住；
    //    若根杆未按规格收窄（半宽=kR），顶点垂直偏移 √2·kR 也会超上限 → 同样被抓住。
    const float perp_limit =
        std::sqrt(2.0f) * kR * kRootRodRadiusScaleSpec + 1e-4f;
    for (size_t k = 0; k < 24; ++k) {
        const Vec3f d(mesh.positions[k].x() - center.x(),
                      mesh.positions[k].y() - center.y(),
                      mesh.positions[k].z() - center.z());
        const float along = d.x() * axis.x() + d.y() * axis.y() + d.z() * axis.z();
        const Vec3f perp(d.x() - along * axis.x(), d.y() - along * axis.y(),
                         d.z() - along * axis.z());
        EXPECT_LE(perp.Norm(), perp_limit)
            << "顶点 " << k << " 偏离骨轴过多（杆朝向错误，或根杆未按规格收窄为 radius/3）";
    }

    // 2) 正向断言：杆沿骨轴确实伸到了 (parent, child) 两端
    //    （避免上面“垂直分量小”但“沿轴伸错”的漏网）。
    float max_along = 0.0f;
    for (size_t k = 0; k < 24; ++k) {
        const Vec3f d(mesh.positions[k].x() - center.x(),
                      mesh.positions[k].y() - center.y(),
                      mesh.positions[k].z() - center.z());
        max_along = std::max(max_along,
                             std::fabs(d.x() * axis.x() + d.y() * axis.y() +
                                       d.z() * axis.z()));
    }
    // |rest_offset| = 0.2√2 ≈ 0.2828，半长 ≈ 0.1414。
    EXPECT_NEAR(max_along, 0.2f * std::sqrt(2.0f) * 0.5f, 1e-4f);
}

// ==================== 3.5 根杆收窄（区分 root）====================

// 根关节的杆半宽 = radius/3（直径 = 其它骨的 1/3）：量测每根杆顶点到骨轴的最大垂直偏移
// （方盒角点处 = √2·半宽）—— 根杆应 ≈ √2·(r/3)、非根杆应 ≈ √2·r。
// 若实现漏掉收窄（根也用 r）或误伤普通杆（都用 r/3），两端会各自被抓住。
TEST(SkeletonMesh, RootRodThinnerThanOthers) {
    // 两骨直链：根（沿 +Y 长 0.5）→ 子（沿 +Y 长 0.4）。两杆轴都平行 +Y。
    SkeletonType type;
    type.joints.resize(2);
    type.joints[0].parent = jpov::kSkeletonNoParent;
    type.joints[0].rest_offset = Vec3f(0.0f, 0.5f, 0.0f);
    type.joints[0].name = "root";
    type.joints[1].parent = 0;
    type.joints[1].rest_offset = Vec3f(0.0f, 0.4f, 0.0f);
    type.joints[1].name = "child";
    type.bind_rotation.clear();
    type.Validate();

    constexpr float kR = 0.06f;
    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, kR);
    ASSERT_EQ(mesh.positions.size(), 48u);  // 2 杆 × 24 顶点

    // 每杆顶点到骨轴（两杆均过原点的 +Y 轴）的最大垂直距离 = 方盒角点 = √2·半宽。
    const auto max_perp_from_y_axis = [&mesh](size_t base) {
        float m = 0.0f;
        for (size_t k = 0; k < 24; ++k) {
            const Vec3f& v = mesh.positions[base + k];
            m = std::max(m, std::sqrt(v.x() * v.x() + v.z() * v.z()));
        }
        return m;
    };
    const float root_max = max_perp_from_y_axis(0);
    const float child_max = max_perp_from_y_axis(24);

    // 根杆：半宽 = kR/3；普通杆：半宽 = kR。期望值用规格硬编码（见 kRootRodRadiusScaleSpec）。
    EXPECT_NEAR(root_max, std::sqrt(2.0f) * kR * kRootRodRadiusScaleSpec, 1e-4f)
        << "根杆半宽应为 radius/3（直径 = 其它骨的 1/3）";
    EXPECT_NEAR(child_max, std::sqrt(2.0f) * kR, 1e-4f)
        << "非根杆半宽应保持 radius";
}

// 顶层零长包装 Root（如 glTF Tripo / 本仓库 Mixamo23）时，「根位置矢量杆收窄 1/3」
// 必须仍然生效 —— 收窄的是**沿根链的第一根可画杆**，不是"parent=无"那个关节本身。
//
// 背景（2026-09-14 Danis 报的 bug）：Mixamo23Skeleton / mixamo_male.glb 的 idx 0 是
// 零长包装 Root（offset=0，本身不画杆），真正从骨架原点指向骨盆的杆是它的子关节 Hips。
// 旧实现按 `parent == kSkeletonNoParent` 判根杆 → 对这类资产**静默失效**，
// Hips 杆按普通骨画（观察器里蓝骨"root 没有变细"就是这么来的）。
TEST(SkeletonMesh, RootRodThinEvenWithZeroLengthWrapperRoot) {
    // 三骨：Root(包装, offset=0) → Hips(沿 +Y 0.5) → Spine(沿 +Y 0.4)。
    SkeletonType type;
    type.joints.resize(3);
    type.joints[0].parent = jpov::kSkeletonNoParent;
    type.joints[0].rest_offset = Vec3f(0.0f, 0.0f, 0.0f);  // 零长包装层
    type.joints[0].name = "";
    type.joints[1].parent = 0;
    type.joints[1].rest_offset = Vec3f(0.0f, 0.5f, 0.0f);  // 根位置矢量
    type.joints[1].name = "mixamorig:Hips";
    type.joints[2].parent = 1;
    type.joints[2].rest_offset = Vec3f(0.0f, 0.4f, 0.0f);  // 真骨骼
    type.joints[2].name = "mixamorig:Spine";
    type.bind_rotation.clear();
    type.Validate();

    constexpr float kR = 0.06f;
    const MeshData mesh =
        jpov::BuildBoneMeshInBoneSpace(type, SkeletonPose::Identity(3), kR);
    // 零长包装 Root 不画杆 → 只有 Hips / Spine 两杆。
    ASSERT_EQ(mesh.positions.size(), 48u);

    const auto max_perp_from_y_axis = [&mesh](size_t base) {
        float m = 0.0f;
        for (size_t k = 0; k < 24; ++k) {
            const Vec3f& v = mesh.positions[base + k];
            m = std::max(m, std::sqrt(v.x() * v.x() + v.z() * v.z()));
        }
        return m;
    };
    // 期望值用**规格硬编码**（1/3），不引用 kRootRodRadiusScale（防恒真断言）。
    EXPECT_NEAR(max_perp_from_y_axis(0), std::sqrt(2.0f) * kR * kRootRodRadiusScaleSpec,
                1e-4f)
        << "🔴 零长包装 Root 之下那根（Hips = 根位置矢量）也应收窄为 radius/3";
    EXPECT_NEAR(max_perp_from_y_axis(24), std::sqrt(2.0f) * kR, 1e-4f)
        << "再下一根（Spine = 真骨）应保持 radius";
}

// ⚠️ 真实 bug 的准确形状（2026-09-14 Danis 报）：glb 顶层 Root 不是"零长"，而是
// **4.5mm 的导出残差**（mixamo_male.glb 实测 0.00452629m）。这种"非零但退化"的杆
// 会抢走根位置矢量的身份 → 变细的是这根看不见的小残杆，真正的 Hips 杆仍是普通粗细。
// 故退化阈值（kDegenerateRodLength=1cm）必须同时用于"跳不跳过"与"谁是根杆"。
TEST(SkeletonMesh, RootRodThinWithTinyStubRoot) {
    // 结构同 mixamo_male.glb：Root(4.5mm 残差) → Hips(0.5) → Spine(0.4)。
    SkeletonType type;
    type.joints.resize(3);
    type.joints[0].parent = jpov::kSkeletonNoParent;
    type.joints[0].rest_offset = Vec3f(0.0f, 0.0045263f, 0.0f);  // 退化残差
    type.joints[0].name = "Root";
    type.joints[1].parent = 0;
    type.joints[1].rest_offset = Vec3f(0.0f, 0.5f, 0.0f);
    type.joints[1].name = "mixamorig:Hips";
    type.joints[2].parent = 1;
    type.joints[2].rest_offset = Vec3f(0.0f, 0.4f, 0.0f);
    type.joints[2].name = "mixamorig:Spine";
    type.bind_rotation.clear();
    type.Validate();

    constexpr float kR = 0.06f;
    const MeshData mesh =
        jpov::BuildBoneMeshInBoneSpace(type, SkeletonPose::Identity(3), kR);
    ASSERT_EQ(mesh.positions.size(), 48u)
        << "退化残差杆不应生成几何 → 只剩 Hips/Spine 两杆";

    const auto max_perp_from_y_axis = [&mesh](size_t base) {
        float m = 0.0f;
        for (size_t k = 0; k < 24; ++k) {
            const Vec3f& v = mesh.positions[base + k];
            m = std::max(m, std::sqrt(v.x() * v.x() + v.z() * v.z()));
        }
        return m;
    };
    EXPECT_NEAR(max_perp_from_y_axis(0), std::sqrt(2.0f) * kR * kRootRodRadiusScaleSpec,
                1e-4f)
        << "🔴 Hips（真正的根位置矢量）应收窄为 radius/3 —— 不能被 4.5mm 残差抢走身份";
    EXPECT_NEAR(max_perp_from_y_axis(24), std::sqrt(2.0f) * kR, 1e-4f)
        << "Spine（真骨）应保持 radius";
}

// 一根杆的"截面半宽" = 顶点到**该杆自身轴线**的最大垂直距离（正确实现 = √2·杆半宽）。
// 轴线 = 父关节位置 → 本关节位置（由 bind JW 算）。对任意骨朝向都成立 ——
// ⚠️ 不能像合成用例那样"量到 Y 轴的距离"：glb 的 Hips 杆被 Root 的 bind 旋转掰了角度，
//    量到 Y 轴会把轴向长度也算进去（实测 0.0270 vs 真值 0.0236，误判为"没收窄"）。
float RodHalfDiagonal(const SkeletonType& type,
                      const std::vector<geom::math::Mat4>& jw,
                      const MeshData& mesh, size_t joint, size_t base) {
    const int par = type.joints[joint].parent;
    const Vec3f a = (par == jpov::kSkeletonNoParent)
                        ? Vec3f(0.0f, 0.0f, 0.0f)
                        : Vec3f(jw[static_cast<size_t>(par)].m[12],
                                jw[static_cast<size_t>(par)].m[13],
                                jw[static_cast<size_t>(par)].m[14]);
    const Vec3f b(jw[joint].m[12], jw[joint].m[13], jw[joint].m[14]);
    const Vec3f axis = (b - a).Unit();
    float m = 0.0f;
    for (size_t k = 0; k < 24; ++k) {
        const Vec3f d = mesh.positions[base + k] - a;
        const Vec3f perp = d - axis * d.Dot(axis);
        m = std::max(m, perp.Norm());
    }
    return m;
}

// 真实资产核对（Danis 报的原始场景：蓝骨 root 不变细）：
// **可见的根位置矢量杆 = mixamorig:Hips 的杆**（fbx/glb/Mixamo23 三套资产都是它），
// 它必须收窄为 radius/3。这条按**骨名**锁定（而不是"第一根杆"—— 那样会被 glb 的
// 4.5mm Root 残差骗过：残差自己也"是根杆且细"，但用户看到的是 Hips 那根）。
TEST(SkeletonMesh, HipsRodThinOnRealSkeletons) {
    constexpr float kR = 0.05f;
    const auto check_hips = [kR](const char* tag, const SkeletonType& type) {
        const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(
            type, SkeletonPose::Identity(type.bone_count()), kR);
        int hips = -1;
        for (size_t j = 0; j < type.joints.size(); ++j) {
            if (type.joints[j].name == "mixamorig:Hips") {
                hips = static_cast<int>(j);
                break;
            }
        }
        ASSERT_GE(hips, 0) << tag << " 应含 mixamorig:Hips";
        // Hips 杆在 mesh 里的顶点基址 = 24 ×（下标在它之前、**已被画出来**的杆数）。
        size_t drawn_before = 0;
        for (int j = 0; j < hips; ++j) {
            if (type.joints[static_cast<size_t>(j)].rest_offset.Norm() >=
                jpov::kDegenerateRodLength) {
                ++drawn_before;
            }
        }
        const std::vector<geom::math::Mat4> jw = ComputeJointWorldBind(type);
        EXPECT_NEAR(RodHalfDiagonal(type, jw, mesh, static_cast<size_t>(hips),
                                    24u * drawn_before),
                    std::sqrt(2.0f) * kR * kRootRodRadiusScaleSpec, 1e-4f)
            << "🔴 " << tag
            << " 的 mixamorig:Hips（可见的根位置矢量杆）应收窄为 radius/3";
    };

    check_hips("Mixamo23Skeleton", jpov::Mixamo23Skeleton(1.75f));

    std::vector<SkeletonType> skins;
    ASSERT_TRUE(jpov::LoadGltfSkeleton(
        jpov::GetTestDataDir() + "/object3d/mixamo_male/mixamo_male.glb", &skins));
    ASSERT_FALSE(skins.empty());
    check_hips("mixamo_male.glb", skins[0]);

    SkeletonType fbx;
    ASSERT_TRUE(jpov::LoadFbxSkeleton(
        jpov::GetTestDataDir() + "/animations/hip_hop_dance.fbx", &fbx));
    check_hips("hip_hop_dance.fbx", fbx);
}

// 第二个骨架（程序化直线骨链）验证通用性：非 Mixamo 骨架也成立。
TEST(SkeletonMesh, WorksOnNonMixamoSkeleton) {
    // 造一个 3 骨直链：root 在原点沿 +Y，第 2 骨沿 +X，第 3 骨沿 +Z。
    // 覆盖"骨长轴沿 ±Z"的退化参考轴分支。
    SkeletonType type;
    type.joints.resize(3);
    type.joints[0].parent = jpov::kSkeletonNoParent;
    type.joints[0].rest_offset = Vec3f(0.0f, 0.2f, 0.0f);
    type.joints[0].name = "A";
    type.joints[1].parent = 0;
    type.joints[1].rest_offset = Vec3f(0.15f, 0.0f, 0.0f);
    type.joints[1].name = "B";
    type.joints[2].parent = 1;
    type.joints[2].rest_offset = Vec3f(0.0f, 0.0f, 0.25f);  // 平行 +Z → 退化分支
    type.joints[2].name = "C";
    type.bind_rotation.clear();  // 全恒等
    type.Validate();

    const SkeletonPose pose = SkeletonPose::Identity(type.bone_count());
    const MeshData mesh = jpov::BuildBoneMeshInBoneSpace(type, pose, 0.02f);
    mesh.Validate();

    EXPECT_EQ(mesh.positions.size(), 24u * 3u);

    // 第 3 骨（沿 +Z）的杆中心应为 C 骨父(B末端)与 C末端的中点。
    // B 末端 = A 的 off(0,0.2,0) + B 的 off(0.15,0,0) = (0.15,0.2,0)
    // C 末端 = (0.15,0.2,0) + (0,0,0.25) = (0.15,0.2,0.25)
    // 杆中心 = (0.15,0.2,0.125)
    const size_t base = 2 * 24;
    Vec3f centroid(0.0f, 0.0f, 0.0f);
    for (size_t k = 0; k < 24; ++k) {
        centroid += mesh.positions[base + k];
    }
    centroid = Vec3f(centroid.x() / 24.0f, centroid.y() / 24.0f,
                     centroid.z() / 24.0f);
    EXPECT_NEAR(centroid.x(), 0.15f, 1e-4f);
    EXPECT_NEAR(centroid.y(), 0.20f, 1e-4f);
    EXPECT_NEAR(centroid.z(), 0.125f, 1e-4f);
}

// ==================== 4. MakeOrientedBox 单测 ====================

TEST(MakeOrientedBox, IdentityRotationMatchesMakeBox) {
    // up=(0,1,0), front=(0,0,1) → 旋转为单位阵，应与 MakeBox 逐字节一致。
    const MeshData a = MeshData::MakeBox(0.1f, 0.2f, 0.3f);
    const MeshData b = MeshData::MakeOrientedBox(0.1f, 0.2f, 0.3f,
                                                 Vec3f(0.0f, 1.0f, 0.0f),
                                                 Vec3f(0.0f, 0.0f, 1.0f),
                                                 Vec3f(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(a.positions.size(), b.positions.size());
    for (size_t i = 0; i < a.positions.size(); ++i) {
        EXPECT_NEAR(a.positions[i].x(), b.positions[i].x(), 1e-6f);
        EXPECT_NEAR(a.positions[i].y(), b.positions[i].y(), 1e-6f);
        EXPECT_NEAR(a.positions[i].z(), b.positions[i].z(), 1e-6f);
    }
}

TEST(MakeOrientedBox, TranslationApplied) {
    const MeshData m = MeshData::MakeOrientedBox(
        0.1f, 0.1f, 0.1f, Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 0.0f, 1.0f),
        Vec3f(1.0f, 2.0f, 3.0f));
    // 24 顶点质心应 = translation。
    Vec3f c(0.0f, 0.0f, 0.0f);
    for (const Vec3f& p : m.positions) {
        c += p;
    }
    EXPECT_NEAR(c.x() / 24.0f, 1.0f, 1e-5f);
    EXPECT_NEAR(c.y() / 24.0f, 2.0f, 1e-5f);
    EXPECT_NEAR(c.z() / 24.0f, 3.0f, 1e-5f);
}

TEST(MakeOrientedBox, NormalsStayUnitLengthAfterRotation) {
    // up/front 非轴对齐 → 旋转后法线仍须单位长（正交化 + 纯旋转保证）。
    const MeshData m = MeshData::MakeOrientedBox(
        0.1f, 0.2f, 0.1f, Vec3f(0.3f, 0.9f, 0.1f), Vec3f(0.0f, 0.1f, 1.0f),
        Vec3f(0.0f, 0.0f, 0.0f));
    for (const Vec3f& n : m.normals) {
        EXPECT_NEAR(n.Norm(), 1.0f, 1e-5f);
    }
}

// ==================== root_offset（root-motion）====================
//
// BuildBoneMeshInBoneSpace 必须把 pose.root_offset 当成**根骨的额外平移**（2026-09-20 接线，
// 与 skeleton_manager.cc 烘焙同一条规则）。不接的话火柴人**只能表现姿态、不能表现位移**
// （原地滑步）—— 而这正是当初拿来 CPU 自查重定向效果的手段，漏接就会看不见 root_offset 的错。
//
// 判据用**顶点质心位移**（而不是 size() 这类恒真量）：注入 root_offset=(0,0.3,0) 后，
// 整堆顶点的质心必须恰好平移 (0,0.3,0)。

// 顶点质心（用已有的 rod 端面顶点，无需借助其它不变量）。
Vec3f MeshCentroid(const MeshData& m) {
    Vec3f c(0.0f, 0.0f, 0.0f);
    for (const Vec3f& p : m.positions) {
        c += p;
    }
    const float n = static_cast<float>(m.positions.size());
    return Vec3f(c.x() / n, c.y() / n, c.z() / n);
}

TEST(SkeletonMesh, RootOffsetTranslatesWholeMesh) {
    const SkeletonType type = jpov::Mixamo23Skeleton(1.75f);
    SkeletonPose base = SkeletonPose::Identity(type.bone_count());
    SkeletonPose moved = base;
    moved.root_offset = Vec3f(0.0f, 0.3f, 0.0f);  // 米

    const MeshData m0 = jpov::BuildBoneMeshInBoneSpace(type, base, 0.04f);
    const MeshData m1 = jpov::BuildBoneMeshInBoneSpace(type, moved, 0.04f);

    ASSERT_EQ(m0.positions.size(), m1.positions.size());
    ASSERT_GT(m0.positions.size(), 0u);
    // 顶点数不能变（只是平移，不是重建）；但对错要看质心。
    const Vec3f c0 = MeshCentroid(m0);
    const Vec3f c1 = MeshCentroid(m1);
    EXPECT_NEAR(c1.x() - c0.x(), 0.0f, 1e-5f) << "root_offset 的 x 分量没生效";
    EXPECT_NEAR(c1.y() - c0.y(), 0.3f, 1e-5f) << "root_offset 的 y 分量没生效";
    EXPECT_NEAR(c1.z() - c0.z(), 0.0f, 1e-5f) << "root_offset 的 z 分量没生效";
}

TEST(SkeletonMesh, ZeroRootOffsetIsByteIdentical) {
    // root_offset = 0（默认）⇒ 与接线前逐顶点一致（零回归）。
    const SkeletonType type = jpov::Mixamo23Skeleton(1.75f);
    SkeletonPose explicit_zero = SkeletonPose::Identity(type.bone_count());
    explicit_zero.root_offset = Vec3f(0.0f, 0.0f, 0.0f);
    const SkeletonPose default_zero = SkeletonPose::Identity(type.bone_count());

    const MeshData a = jpov::BuildBoneMeshInBoneSpace(type, explicit_zero, 0.04f);
    const MeshData b = jpov::BuildBoneMeshInBoneSpace(type, default_zero, 0.04f);
    ASSERT_EQ(a.positions.size(), b.positions.size());
    for (size_t i = 0; i < a.positions.size(); ++i) {
        EXPECT_FLOAT_EQ(a.positions[i].x(), b.positions[i].x());
        EXPECT_FLOAT_EQ(a.positions[i].y(), b.positions[i].y());
        EXPECT_FLOAT_EQ(a.positions[i].z(), b.positions[i].z());
    }
}

}  // namespace

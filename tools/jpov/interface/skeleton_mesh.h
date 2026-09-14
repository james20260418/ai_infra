// JPOV Skeleton — 骨架可视化（火柴人 mesh）生成：CPU 侧、GL-free
//
// 目的：由一份 SkeletonType 造出**能直接 RegisterMesh 并蒙皮渲染**的"火柴人" MeshData ——
//   每根骨用一根细长杆（盒）表示，杆即该骨在 bind 姿态下的 rest_offset 矢量。
//   用途（见 docs/jpov_retarget_design.md §4）：
//     1. 人工校准工具的对齐参照 —— 把 glb mesh 摆到与火柴人重合，即可肉眼判断是否贴骨；
//     2. 调通 FBX 动画预览的载体 —— 先用火柴人验证动作对不对，再上真皮。
//
// 坐标约定（重要，见 docs/jpov_retarget_design.md §2.1 / §4.2）：
//   顶点坐标 = 各关节在**骨架空间**（skeleton space）下的 bind 姿态坐标。
//   骨架空间 = 骨架自身坐标系：根关节位于 (0,0,0)，骨沿局部 +Y 生长。
//   即：本函数输出的 mesh 是"骨架空间下的 T-pose 火柴人"——把它与同处 identity 摆放的
//   资产 mesh 叠在一起，两者应重合（这正是 retarget 人工配准的前提）。
//
// 蒙皮配套（§4.5）：
//   火柴人顶点姿态 == SkeletonType 的 rest（两边同源同值），故 inverse_bind = JW_bind⁻¹
//   由 SkeletonType::ComputeInverseBind() **自算必然正确** —— 本函数是"程序化生成走自算"
//   路线的第一个实践者。（对比：Tripo glb 等外部资产必须用其自带的 inverse_bind。）
//
// 职责边界同 mesh.h / skeleton_types.h：本文件只做 CPU 几何生成，不碰 GL、不注册资源。

#ifndef JPOV_INTERFACE_SKELETON_MESH_H_
#define JPOV_INTERFACE_SKELETON_MESH_H_

#include <array>
#include <cstdint>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "geom/math/mat4.h"

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 火柴人骨的默认半宽（米）。骨的"大致直径"≈ 2·radius。
// 0.04 半宽 ⇒ 直径 8cm 的杆，在 1.75m 身高下肉眼可辨（见 Danis 定调）。
inline constexpr float kDefaultBoneRadius = 0.04f;

// 退化杆长度阈值（米）：短于此的杆视为**零长包装层的数值残差**，不画（也不参与"谁是根杆"判定）。
//
// 依据（2026-09-14 实测）：`mixamo_male.glb` 顶层 `Root` 的 rest_offset 只有 **4.5mm**
//（Tripo 导出残差），若照画，它会抢走"根位置矢量"的身份 —— 变细的是这根看不见的小残杆，
// 而真正从骨架原点指向骨盆的 `Hips` 杆（0.53m）仍是普通粗细（Danis 报的"蓝骨 root 没变细"）。
// 取值依据：本仓库人形资产的最小**真骨** = **21mm**（fbx 手指）> 10mm > **4.5mm**（glb Root 残差），
// 两侧余量都 >2×。假设骨架为**米制人形尺度（≥1m）**；亚 1cm 真骨的微型资产需调小此值。
inline constexpr float kDegenerateRodLength = 0.01f;

// 根关节杆（= "根位置矢量"，不是真骨骼段）的半宽缩放：直径 = 其它骨的 1/3
//（视觉区分 root，2026-09-14 Danis 定）。
// ⚠️ 适用对象是**沿根链的第一根可画杆**，不是"parent=无"那个关节本身 ——
//    很多资产（glTF Tripo、本仓库 Mixamo23）在顶层套了一层**零长包装 Root**
//（offset=0、本身不画杆），真正"从骨架原点到骨盆"的那根杆是它的子关节（Hips）。
//    详见 BuildBoneMeshInBoneSpace 里 is_root_rod 的说明。
inline constexpr float kRootRodRadiusScale = 1.0f / 3.0f;

// 把一个"已带骨索引语义"的杆盒 mesh 追加进目标 MeshData。
//
// 用于把逐骨生成的杆拼成一个整体 mesh —— 这是**骨语义的拼接**，不是通用的 mesh 合并：
//   - positions / normals 直接 append；
//   - joint_indices 每顶点填同一个 joint（4 组全同，单骨影响）；
//   - joint_weights 每顶点填 {1,0,0,0}（权重全给该骨）；
//   - 源 mesh 的 indices 统一加当前 dst 顶点数基址后 append（索引重映射）。
//
// Pre-condition: src 已带 kPosition + kNormal，且 src.indices 中每个值 < src 顶点数；
//   joint ∈ [0, dst 所属骨架 bone_count)。
//   调用方负责保证 dst 的 flags 已含 kPosition|kNormal|kJoints（不一致会被 Validate() 拦下）。
inline void AppendBoneBox(MeshData* dst, const MeshData& src, int32_t joint) {
    CHECK(dst != nullptr) << "AppendBoneBox: dst 不能为空";
    CHECK_GE(joint, 0) << "AppendBoneBox: joint 索引不能为负，收到 " << joint;
    CHECK(MeshHasFlag(src.flags, MeshVertexFlags::kPosition))
        << "AppendBoneBox: src 必须含 kPosition";
    CHECK(MeshHasFlag(src.flags, MeshVertexFlags::kNormal))
        << "AppendBoneBox: src 必须含 kNormal（火柴人法线用于光照）";

    const uint32_t base = static_cast<uint32_t>(dst->positions.size());
    const uint32_t src_vcount = static_cast<uint32_t>(src.positions.size());

    for (const Vec3f& p : src.positions) {
        dst->positions.push_back(p);
    }
    for (const Vec3f& n : src.normals) {
        dst->normals.push_back(n);
    }
    for (size_t i = 0; i < src.positions.size(); ++i) {
        dst->joint_indices.push_back(
            std::array<int32_t, 4>{joint, joint, joint, joint});
        dst->joint_weights.push_back(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
    }
    for (uint32_t idx : src.indices) {
        // 防住手工构造的坏 mesh：越界索引重映射后会指向 dst 里其它骨的顶点（静默错位）。
        CHECK_LT(idx, src_vcount)
            << "AppendBoneBox: src.indices 越界（" << idx << " >= " << src_vcount << "）";
        dst->indices.push_back(base + idx);
    }
}

// 由骨架定义 + 姿态造一个"火柴人" mesh（骨架空间下的 T-pose）。
//
// 生成规则（docs/jpov_retarget_design.md §4.2，Danis 定）：
//   - 每根骨 = 一根细长杆（盒），杆表示该骨在 bind 姿态下的 rest_offset **矢量**：
//       · 杆长（沿骨长轴）= |rest_offset|（mod 长）
//       · 杆中心 = rest_offset 的中点（父关节局部系下 = rest_offset/2）
//       · 横截面 = radius × radius（两个垂直方向各取 radius 作半宽 ⇒ 直径 ≈ 2·radius）
//   - 杆在**父关节局部坐标系**里造（长轴沿该骨的骨长朝向），再沿骨架树复合变换到骨架空间。
//   - 根关节位于 (0,0,0)（骨架空间原点）。
//   - **根位置矢量杆**（沿根链的第一根可画杆；见 kRootRodRadiusScale 的说明）半宽收窄为
//     radius/3（直径 = 其它骨的 1/3）——视觉上区分 root；
//     其余骨一律 radius。
//
// 骨长轴朝向：以该骨 bind 姿态下的**世界朝向**为准（即 JW_bind 的旋转部分作用于局部 +Y）。
//   骨长轴之外的另一轴（垂直于杆的"左/前"）取同一旋转作用于局部 +Z，
//   两者共同确定杆的朝向（见 MeshData::MakeOrientedBox 的 up/front 语义）。
//
// 输出：MeshData{ flags = kPosition|kNormal|kJoints }，可直接 RegisterMesh。
//   顶点坐标 = 骨架空间 bind 坐标；joint_indices 指向骨索引；joint_weights = {1,0,0,0}。
//
// 参数：
//   type   —— 骨架定义（树 + rest_offset + bind_rotation）。须已 Validate()。
//   pose   —— 骨骼姿态；决定每骨的朝向。送 SkeletonPose::Identity(type.bone_count())
//             即得 T-pose 火柴人（bind 朝向已烘进 SkeletonType，pose 不再叠旋转）。
//             须满足 pose.bone_count == type.bone_count()。
//   radius —— 骨杆半宽（米）。必须 > 0，非法值 LOG(FATAL)（不 fallback，见工程约定）。
//
// Pre-condition: type.Validate() 通过；pose.bone_count == type.bone_count()；radius > 0。
inline MeshData BuildBoneMeshInBoneSpace(const SkeletonType& type,
                                         const SkeletonPose& pose,
                                         float radius = kDefaultBoneRadius) {
    type.Validate();
    CHECK_EQ(pose.bone_count, type.bone_count())
        << "BuildBoneMeshInBoneSpace: pose.bone_count(" << pose.bone_count
        << ") 必须 == type.bone_count(" << type.bone_count() << ")";
    CHECK_GT(radius, 0.0f)
        << "BuildBoneMeshInBoneSpace: radius(半宽,米) 必须 >0，收到 " << radius;

    const size_t n = type.joints.size();
    // pose.joint_rotation 为空 = 走 rest（隐式全恒等），与 skeleton_types.h 语义一致。
    if (!pose.joint_rotation.empty()) {
        CHECK_EQ(pose.joint_rotation.size(), n)
            << "BuildBoneMeshInBoneSpace: pose.joint_rotation 尺寸 "
            << pose.joint_rotation.size() << " 应 == 骨数 " << n;
    }

    // 1) 沿拓扑序复合出每关节在骨架空间下的 bind 局部→空间变换 JW。
    //    JW[j] = JW[parent] · T(rest_offset[j]) · R(bind_rotation[j]) · R(pose_rotation[j])
    //    pose 恒等时 JW[j] = JW_bind[j]（= T-pose），与 ComputeInverseBind() 同源同值。
    std::vector<geom::math::Mat4> jw(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> bind =
            type.bind_rotation.empty() ? geom::Quaternion<float>::Identity()
                                       : type.bind_rotation[j];
        const geom::Quaternion<float> rot =
            pose.joint_rotation.empty() ? geom::Quaternion<float>::Identity()
                                        : pose.joint_rotation[j];
        const geom::math::Mat4 local =
            geom::math::JointLocal(type.joints[j].rest_offset, bind, rot);
        const int p = type.joints[j].parent;
        if (p == kSkeletonNoParent) {
            jw[j] = local;
        } else {
            CHECK_GE(p, 0);
            jw[j] = geom::math::Mat4Mul(jw[static_cast<size_t>(p)], local);
        }
    }

    // 2) 逐骨造杆并追加。杆在父关节局部系里造：
    //    起点 = 父关节局部原点（根的父系 = 骨架空间原点），长轴 = 父系下的 rest_offset 方向。
    //    ⚠️ 用**父系**而非本关节系：rest_offset 的定义就是"相对父关节的局部平移"，
    //    沿它方向造杆、再整体搬进骨架空间，杆两端自然落在父关节与子关节上。

    // 预先标出"根位置矢量杆"：沿父链向上直到根，若**没有任何一根可画的杆**（长度 > 0），
    // 则本杆就是根位置矢量。
    // ⚠️ 判据不能用 `parent == kSkeletonNoParent`：那要求根关节**自己带长度**；而
    //    glTF Tripo / 本仓库 Mixamo23 的顶层是**零长包装 Root**（offset=0，不画杆），
    //    真正从骨架原点指向骨盆的杆是它的子关节 Hips → 旧判据会让这根杆按普通骨画，
    //    "root 杆收窄 1/3"对这类资产静默失效（2026-09-14 Danis 报的 bug）。
    std::vector<bool> is_root_rod(n, false);
    for (size_t j = 0; j < n; ++j) {
        if (type.joints[j].rest_offset.Norm() < kDegenerateRodLength) {
            continue;  // 自身是退化杆（不画）
        }
        bool first_drawable = true;
        for (int p = type.joints[j].parent; p != kSkeletonNoParent;
             p = type.joints[static_cast<size_t>(p)].parent) {
            CHECK_GE(p, 0);
            CHECK_LT(p, static_cast<int>(j))  // 拓扑序（Validate 已保证，此处防御）
                << "BuildBoneMeshInBoneSpace: joint[" << j << "] 的父链非拓扑序";
            if (type.joints[static_cast<size_t>(p)].rest_offset.Norm() >=
                kDegenerateRodLength) {
                first_drawable = false;  // 父链上已有可画的杆 → 本杆不是根位置矢量
                break;
            }
        }
        is_root_rod[j] = first_drawable;
    }
    MeshData out;
    out.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kJoints));

    for (size_t j = 0; j < n; ++j) {
        const Vec3f& off = type.joints[j].rest_offset;
        const float len = off.Norm();
        // 退化骨（零长包装层 Root，或 glb 那种 4.5mm 的导出残差）：没有"杆"可画，跳过。
        // 不是错误 —— 包装层本就不占几何（阈值见 kDegenerateRodLength）。
        if (len < kDegenerateRodLength) {
            continue;
        }

        // 父关节的骨架空间变换（根则用单位变换，即骨架空间自身）。
        const int p = type.joints[j].parent;
        geom::math::Mat4 parent_xform = geom::math::Mat4Identity();
        if (p != kSkeletonNoParent) {
            CHECK_GE(p, 0);
            parent_xform = jw[static_cast<size_t>(p)];
        }

        // 根位置矢量杆（父链上无任何可画杆 → 见上方 is_root_rod）半宽收窄为 radius/3。
        const float rod_radius =
            is_root_rod[j] ? radius * kRootRodRadiusScale : radius;

        // 杆的朝向（父系下的局部轴）：长轴 = rest_offset 单位化。
        // 参考轴（决定杆的横截面哪个朝向是"前"）：取局部 +Z（骨 up 惯例，见 §2.1）。
        // ⚠️ 若骨长轴本身近乎平行于 +Z（如沿 ±Z 长的骨），则 up/front 平行 → cross 退化。
        //    此时改用 +Y 作参考轴。横截面的"旋转相位"对圆截面无影响（正方向与左方向
        //    半宽都是 radius），故换参考轴不改变杆的可见形状，仅避开退化。
        //    这使本函数对任意骨架（不只 Mixamo23）都安全。
        const Vec3f bone_dir = off.Unit();
        const float dir_cos_z = std::fabs(bone_dir.z());
        const Vec3f ref_up = (dir_cos_z > 0.9f) ? Vec3f(0.0f, 1.0f, 0.0f)
                                                : Vec3f(0.0f, 0.0f, 1.0f);

        // 杆中心（父系下）= rest_offset 中点。
        const Vec3f center_local(off.x()*0.5f, off.y()*0.5f, off.z()*0.5f);

        // 先在父系造杆（中心在 center_local、长轴沿 bone_dir、半长 = len/2、截面半宽 = rod_radius），
        // 再用父关节变换把它搬进骨架空间。
        MeshData rod = MeshData::MakeOrientedBox(
            /*front_half_width*/ rod_radius, /*up_half_width*/ len * 0.5f,
            /*left_half_width*/ rod_radius, /*up*/ bone_dir, /*front*/ ref_up,
            /*translation*/ center_local);

        // rod 的顶点此刻在父系；施加父关节变换 → 骨架空间。
        // （父变换是刚体变换，法线用左上 3x3 旋转部分即可；此处 JW 由正交旋转+平移构成。）
        for (Vec3f& pos : rod.positions) {
            pos = geom::math::Mat4TransformPoint(parent_xform, pos);
        }
        for (Vec3f& nrm : rod.normals) {
            nrm = geom::math::Mat4TransformVector(parent_xform, nrm);
        }

        AppendBoneBox(&out, rod, static_cast<int32_t>(j));
    }

    out.Validate();
    return out;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_MESH_H_

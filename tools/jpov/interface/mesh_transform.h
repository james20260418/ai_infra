// JPOV mesh_transform — 把「绘制时的放置参数」烘进 CPU 资产（GL-free）
//
// 用途：模型编辑器把用户摆好的 (center, up, front, scale) 保存成新 glb 时，
//   需要把这份**放置变换**烘进 CPU 侧的几何与骨架，产出"原地就是摆好姿态"
//   的资产，使保存出来的 glb 用**静态管线**（DrawObject3D 系）打开即为屏幕所见。
//
// 为什么不用「保存时把参数写进文件、打开时再施加」：glTF 的 node TRS 会被
//   loader 丢弃（只取平移+缩放，见 gltf_loader.cc），且骨架/网格的坐标契约
//   不统一 → 保存的参数在重载后不保证复原。**烘进顶点**是唯一自洽的路径。
//
// ═══════════ 变换语义（与 object3d 的 BuildModelMatrix 严格对齐）═══════════
//   v_world = T(center) · R(up,front) · (scale · v_local)
//   n_world = normalize( R(up,front) · (scale · n_local) )
//   t_world = scale · ( R(up,front) · t_local )      // 切线不归一化（同 VS）
//
// 记 U = T(center)·R(up,front)，即"不含 scale 的刚体放置矩阵"。**骨架烘焙只支持
// 刚体 U（scale 必须 == 1.0f）**：缩放对骨架的烘焙不闭合（见下方"骨架一致性"），
// 而需求验收路径是"旋转一个 glb → 保存 → viewer 打开保留旋转"，旋转即刚体。
// 顶点烘焙对 scale 无此限制（缩几何永远良定义）。
//
// ═══════════ 骨架一致性（本文件存在的核心理由）═══════════
// 蒙皮链是：pose(相对父旋转) → 沿树解算每关节 JointMatrix M_j → 用各骨的刚体蒙皮变换
//   作用于顶点（v = Σ w_j·M_j·v_local；**同一混合的刚体形式**即骨架链路，2026-09-18 起
//   渲染侧以对偶四元数实现，见 geom/math/dual_quat.h —— 下面推导只用到「每骨一个刚体变换
//   且对权重线性」两条性质，与用矩阵还是对偶四元数表示无关），
//   最后再乘 uModel 得世界坐标。若只把顶点烘了 U 而骨架不动，则结果为
//   Σ w_j·M_j·(U·v) = (Σ w_j·M_j·U)·v，而正确结果应是 U·Σ w_j·M_j·v —— 两者只在
//   U 与所有 M_j 可交换时才相等，一般不等（表现为"旋转后的带骨模型存下来 pose 跑掉"）。
//
// 修正：把 U 烘进**骨架空间**（而非只烘顶点），目标是让每关节的 bind 世界矩阵满足
//     JW_bind'[j] = U · JW_bind[j] · U⁻¹        （j = 全部关节，含根）
//   记 R = R(up,front)、t = center，U = [R | t]（刚体）。jointLocal(j) = T(o_j)·R(b_j)，
//   沿树复合即得 JW_bind[j]。要让复合结果满足上式，逐个关节的 local 必须满足
//     T(o'_j)·R(b'_j) = U · T(o_j)·R(b_j) · U⁻¹
//
//   一般式（对**每个**关节都成立，与是否根无关）：
//     T(o'_j)·R(b'_j) = U · T(o_j)·R(b_j) · U⁻¹
//     ⇒  b'_j = q_R · b_j · q_R⁻¹
//        o'_j = R · o_j + center − R(b'_j) · center
//   ⚠️ **每个关节**都带 `center − R(b'_j)·center` 这一修正项（不止根！）：U 含平移 t，
//      用 t 对 `T(o)·R(b)` 做共轭时，平移项 `R(b)` 会把 t 转掉，需减回。
//      （2026-09-13 单测抓到：最初只给根加该项，子骨偏差 1.69。）
//   仅在 b_j == I（该骨无 bind 旋转）时，R(b'_j)=I ⇒ 修正项 = 0 ⇒ 退化为 o'_j = R·o_j。
//   若整个骨架 bind_rotation 为空（极简直链），则修正项全为 0，只剩根的 +center。
//
//   ⚠️ **更关键的前提：资产必须处于 rest 姿态（pose 恒等）。** 关节 local 完整形式是
//      `T(o)·R(b)·R(pose)`；若 pose 非恒等，共轭后偏差项会变成 `center − R(b_j)·R(pose_j)·center`，
//      且 pose 自身也需共轭（pose' = q_R pose q_R⁻¹）。本函数**只按 R(b_j) 算偏差**，
//      因此**只对 rest 姿态正确**。保存场景存的就是 rest 姿态，故这是设计内的：
//      本函数**不接 pose 参数**，拒绝"烘带 pose 的资产"（避免静默漏算 pose 项）。
//
//   如此逐关节满足局部等式 → 沿树复合严格得 JW'[j] = U · JW[j] · U⁻¹，从而
//     蒙皮结果 = Σ w·(U·JW·U⁻¹)·(U·IBM·U⁻¹)·(U·v) = U · Σ w·JW·IBM·v = U·(原结果) ✅
//   （IBM' = (JW_bind')⁻¹ = U·JW_bind⁻¹·U⁻¹ = U·IBM·U⁻¹ 自动成立）。
//
//   ⚠️ **前提：资产必须处于 bind/rest 姿态（pose 恒等）。** 保存场景存的就是 rest 姿态，
//      故本函数不接 pose 参数——不提供"烘带 pose 的资产"（见上方推导：带 pose 需另算偏差项）。
//
// 因此：**顶点与骨架同时烘同一个 U，几何与 pose 都保留**。
//
// ═══════════ 纯函数约定 ═══════════
// 本文件是 GL-free 纯函数层：不改入参、返回新对象，便于"保存时才算、编辑态不动"。

#ifndef JPOV_INTERFACE_MESH_TRANSFORM_H_
#define JPOV_INTERFACE_MESH_TRANSFORM_H_

#include <cmath>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// ==================== 朝向基构造 ====================

// 放置矩阵的**旋转基**：由 (up, front) 重建标准正交基 (left, up, front)。
//
// 与 object3d_renderer 的 BuildModelMatrix / MeshData::MakeOrientedBox 同一套约定：
//   local +Y → up, local +Z → front, local +X → left = normalize(cross(up, front))。
//
// 对 front 做 Gram-Schmidt 正交化（减掉在 up 上的投影）：编辑器增量旋转下 up/front
// 理论上恒正交，但浮点累积 + 外部直接赋值可能引入微小不垂直；不正交会在烘顶点时
// 引入剪切（隐藏的形变），故此处显式正交化。
struct PlacementBasis {
    Vec3f left;   // normalize(cross(up, front))
    Vec3f up;     // 归一化后的 up
    Vec3f front;  // 正交化 + 归一化后的 front
};

// Pre-condition: up / front 非零且不平行（cross 非退化），否则 LOG(FATAL)。
inline PlacementBasis MakePlacementBasis(const Vec3f& up, const Vec3f& front) {
    const float u_len = std::sqrt(up.x() * up.x() + up.y() * up.y() +
                                  up.z() * up.z());
    CHECK_GT(u_len, 1e-8f) << "MakePlacementBasis: up 向量不能为零";
    const Vec3f upn(up.x() / u_len, up.y() / u_len, up.z() / u_len);

    const float f_len = std::sqrt(front.x() * front.x() + front.y() * front.y() +
                                  front.z() * front.z());
    CHECK_GT(f_len, 1e-8f) << "MakePlacementBasis: front 向量不能为零";
    const Vec3f frn(front.x() / f_len, front.y() / f_len, front.z() / f_len);

    // left = normalize(cross(upn, frn))
    Vec3f l(upn.y() * frn.z() - upn.z() * frn.y(),
            upn.z() * frn.x() - upn.x() * frn.z(),
            upn.x() * frn.y() - upn.y() * frn.x());
    const float l_len = std::sqrt(l.x() * l.x() + l.y() * l.y() + l.z() * l.z());
    CHECK_GT(l_len, 1e-8f)
        << "MakePlacementBasis: up 与 front 平行（cross 退化），无法确定朝向";
    l = Vec3f(l.x() / l_len, l.y() / l_len, l.z() / l_len);

    // front 正交化：减去在 upn 上的投影。
    const float proj = frn.x() * upn.x() + frn.y() * upn.y() + frn.z() * upn.z();
    Vec3f f_orth(frn.x() - proj * upn.x(), frn.y() - proj * upn.y(),
                 frn.z() - proj * upn.z());
    const float fo_len = std::sqrt(f_orth.x() * f_orth.x() +
                                   f_orth.y() * f_orth.y() +
                                   f_orth.z() * f_orth.z());
    CHECK_GT(fo_len, 1e-8f)
        << "MakePlacementBasis: up 与 front 近乎平行，正交化后退化";
    f_orth = Vec3f(f_orth.x() / fo_len, f_orth.y() / fo_len, f_orth.z() / fo_len);

    PlacementBasis basis;
    basis.left = l;
    basis.up = upn;
    basis.front = f_orth;
    return basis;
}

// 用旋转基转动矢量（R·v）：局部系 → 世界系方向。
// R 的列 = (left, up, front)（因 R·(1,0,0)=left、R·(0,1,0)=up、R·(0,0,1)=front）。
inline Vec3f RotateByBasis(const PlacementBasis& b, const Vec3f& v) {
    return Vec3f(b.left.x() * v.x() + b.up.x() * v.y() + b.front.x() * v.z(),
                 b.left.y() * v.x() + b.up.y() * v.y() + b.front.y() * v.z(),
                 b.left.z() * v.x() + b.up.z() * v.y() + b.front.z() * v.z());
}

// 由标准正交旋转基构造单位四元数（用于 bind_rotation 共轭）。
// 用 Shepperd 法（选最大对角元分支）避免 trace 接近 0 时的数值不稳。
inline geom::Quaternion<float> QuaternionFromBasis(const PlacementBasis& b) {
    // 3x3 元素（列主序）：col0=left, col1=up, col2=front。
    const float m00 = b.left.x(),  m10 = b.left.y(),  m20 = b.left.z();
    const float m01 = b.up.x(),    m11 = b.up.y(),    m21 = b.up.z();
    const float m02 = b.front.x(), m12 = b.front.y(), m22 = b.front.z();
    const float trace = m00 + m11 + m22;

    geom::Quaternion<float> q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;   // s = 4w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;   // s = 4x
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;   // s = 4y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;   // s = 4z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    q.NormalizeInPlace();
    return q;
}

// 共轭旋转：q · b · q⁻¹（q 为单位四元数）。把 b 表达的朝向换到 q 之后的新基。
inline geom::Quaternion<float> QuaternionConjugateRotate(
    const geom::Quaternion<float>& q,
    const geom::Quaternion<float>& b) {
    return q * b * q.Conjugate();   // 单位四元数：逆 = 共轭
}

// ==================== ③ Apply：把放置参数烘进 CPU mesh ====================

// 把放置参数烘进 MeshData（返回新 mesh，不改入参）。
//
// 公式（见文件头）：
//   v' = center + R·(scale · v)
//   n' = R·n                     // R 正交且 scale 对法线是恒等（同 kMeshVs3dPBRFull）；
//                                // 输入 n 已单位（loader 已归一）故输出仍单位，无需再 normalize
//   t' = scale · (R·t)           // 切线不归一化（与 VS 一致）
//   uvs / indices / joint_indices / joint_weights / flags 原样
//
// Pre-condition: in.Validate() 通过；scale > 0；up/front 非零不平行。
// 说明：本函数**不要求** scale == 1——顶点烘焙对任意 scale 良定义；只有骨架烘焙
//   （ApplyPlacementToSkeleton）才要求 scale == 1（见文件头"骨架一致性"）。
MeshData ApplyPlacementToMesh(const MeshData& in,
                              const Vec3f& center,
                              const Vec3f& up,
                              const Vec3f& front,
                              float scale) {
    in.Validate();
    CHECK_GT(scale, 0.0f) << "ApplyPlacementToMesh: scale 必须 > 0";
    const PlacementBasis b = MakePlacementBasis(up, front);

    MeshData out = in;   // 拷贝：uvs/indices/joint_* / flags 原样保留

    for (Vec3f& p : out.positions) {
        const Vec3f s(p.x() * scale, p.y() * scale, p.z() * scale);
        const Vec3f r = RotateByBasis(b, s);
        p = Vec3f(r.x() + center.x(), r.y() + center.y(), r.z() + center.z());
    }
    for (Vec3f& n : out.normals) {
        n = RotateByBasis(b, n);   // 法线不缩放（见公式）
    }
    for (Vec3f& t : out.tangents) {
        const Vec3f s(t.x() * scale, t.y() * scale, t.z() * scale);
        t = RotateByBasis(b, s);
    }
    return out;
}

// 把放置参数烘进 SkeletonType（返回新骨架，不改入参）。语义见文件头"骨架一致性"。
//
//   rest_offset'[j]    = R·rest_offset[j] + center − R(b'_j)·center      (全部关节，统一式)
//   bind_rotation'[j]  = q_R · bind_rotation[j] · q_R⁻¹                 (bind_rotation 空则保持空)
//   joints[].parent / name 原样
//   （b_j == I 时修正项为 0；全骨架 bind_rotation 为空时只剩根多一个 +center。）
//
// Pre-condition: in.Validate() 通过；up/front 非零不平行。
// 说明：**本函数按刚体 U 烘焙（scale 不由它承担）**。调用方若要保存缩放，
//   必须先把 scale 烘进顶点（ApplyPlacementToMesh），并把骨架的 rest_offset 也
//   按同一 scale 缩放——本函数不接 scale 参数，避免"骨架被非刚体变换"的静默失真。
SkeletonType ApplyPlacementToSkeleton(const SkeletonType& in,
                                      const Vec3f& center,
                                      const Vec3f& up,
                                      const Vec3f& front) {
    in.Validate();
    const PlacementBasis b = MakePlacementBasis(up, front);
    const geom::Quaternion<float> q_r = QuaternionFromBasis(b);

    SkeletonType out = in;

    // 先算 bind_rotation'（下面逐关节算 rest_offset 的修正项需要用到 b'_j）。
    if (!out.bind_rotation.empty()) {
        for (size_t j = 0; j < out.bind_rotation.size(); ++j) {
            out.bind_rotation[j] =
                QuaternionConjugateRotate(q_r, in.bind_rotation[j]);
        }
    }

    for (size_t j = 0; j < out.joints.size(); ++j) {
        const Vec3f rotated = RotateByBasis(b, in.joints[j].rest_offset);
        // 平移修正项 center − R(b'_j)·center（见文件头推导；b 恒等时为 0）。
        const geom::Quaternion<float> b_prime =
            (out.bind_rotation.empty() || j >= out.bind_rotation.size())
                ? geom::Quaternion<float>::Identity()
                : out.bind_rotation[j];
        const Vec3f bc = geom::RotateVector(b_prime, center);
        out.joints[j].rest_offset =
            Vec3f(rotated.x() + center.x() - bc.x(),
                  rotated.y() + center.y() - bc.y(),
                  rotated.z() + center.z() - bc.z());
    }
    return out;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_MESH_TRANSFORM_H_

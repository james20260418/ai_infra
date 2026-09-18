// dual_quat.h — 刚体变换的对偶四元数（DQS 蒙皮基元，GL-free 纯数学层）
//
// ═══════════ 为什么需要它 ═══════════
// 蒙皮要把「每根骨各自的刚体变换」按顶点权重**混合**成顶点实际受到的变换。矩阵混合
// （LBS，线性混合蒙皮）算的是 Σ w_i·M_i —— 一堆旋转矩阵的加权**平均一般不是旋转**
// （正交性被破坏）⇒ 关节弯折处的顶点被“拉向弦”，表现为体积塌陷 / 扭转时糖纸（candy
// wrapper）伪影。对偶四元数把刚体变换打包成一个 8 元数（旋转 + 平移一起），线性混合
// 再归一化**仍然是刚体** ⇒ 混合出的姿态不塌不拧（Dual Quaternion Linear Blending /
// DLB；Kavan et al. 2007 "Skinning with Dual Quaternions"）。
//
// ═══════════ 数学定义（本文件即权威；注释与实现必须一致）═══════════
// 记单位四元数 r（旋转）、平移向量 v，刚体变换 T(p) = r·p·r* + v。对偶四元数（ε² = 0）
//     q̂ = r + ε·t
//   实部 q = r 就是旋转；**对偶部**取
//     t = ½ · v̂ ⊗ r        （v̂ = (0, v) 的纯四元数，⊗ 为 Hamilton 乘法，见 quaternion.h）
//   反过来（本文件 DualQuatTranslationOf 的推导）：
//     t ⊗ r* = ½·v̂ ⊗ r ⊗ r* = ½·v̂   ⇒   v = 2 · vec(t ⊗ r*)
//   于是变换写作
//     T(p) = r·p·r* + 2·vec(t ⊗ r*)
//
// GLSL/VS 里用等价展开式（避免在着色器里构矩阵，也避免四元数乘法函数）：
//     p' = p + 2·r.w·(r.xyz × p) + 2·(r.xyz × (r.xyz × p))     ← 旋转部分（同 RotateVector）
//          + 2·(r.w·t.xyz − t.w·r.xyz + r.xyz × t.xyz)          ← 平移部分
//   （两者等价：把 t ⊗ r* 的向量部展开即得，见 dual_quat_test.cc 的交叉验证。）
//
// ═══════════ 两条必须遵守的约束 ═══════════
// 1) **归一化**：单位对偶四元数满足 |r| = 1 且 r·t = 0（正交约束）。加权和 Σ w_i·q̂_i
//    一般两者都被破坏 ⇒ 混合后必须归一化：实部/对偶部**同除 |Σ w_i·r_i|**
//    （DualQuatNormalized；只归一化实部会让平移整体缩放错）。
// 2) **抗对偶 antipodality**：q̂ 与 −q̂ 表示**同一个**刚体变换，但两者相加会互相抵消
//    ⇒ 混合前必须把每个 q̂ 与「参考」统一到同一半球（dot(r_i, r_ref) < 0 → (r_i,t_i)
//    整体取负）。参考不要靠“猜”，由调用方给出：工程里取**权重最大的那根骨**
//    （逐顶点确定，与姿态/时间轴无关）——见 DualQuatBlendWithReference 的 reference_index。
//
// ═══════════ 与本工程矩阵链路的关系 ═══════════
// 骨架链路 jointWorld = T(rest_offset)·R(bind)·R(pose) 沿树复合、inverseBind 为其逆
// （geom/math/mat4.h JointLocal / SkeletonType::ComputeInverseBind）——**只含旋转与平移**,
// 恒为**刚体**。因此「刚体矩阵 ⇄ 对偶四元数」在本工程里是**无损**的两种写法（不是近似），
// 换成 DQS 不丢任何信息。反之：含缩放/剪切的矩阵**没有**对应的单位对偶四元数，
// DualQuatFromRigidMatrix 对此直接 LOG(FATAL)（不 fallback，见工程约定）。
//
// ═══════════ 命名与约定 ═══════════
//   - 命名空间 geom::math（与 mat4.h 同层）；用 geom::Quaternion<float> 的 (x,y,z,w) 存储。
//   - DualQuatMultiply(a, b) 语义 = 「先作用 b、再作用 a」，与 Mat4Mul 一致。
//   - 不用运算符重载（同 mat4.h 的取舍：矩阵/对偶四元数乘法不满足交换律，具名更直白）。
//   - 本模块**不碰 GL**：GPU 侧只有一份“同公式”的 GLSL（src/skeleton/skinning_shader.h），
//     两侧由测试逐像素对齐（见 test/skeleton/jpov_skinned_multipose_test.cc）。

#ifndef GEOM_MATH_DUAL_QUAT_H_
#define GEOM_MATH_DUAL_QUAT_H_

#include <cmath>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "geom/math/mat4.h"

namespace geom {
namespace math {

// 刚体变换的对偶四元数 q̂ = q + ε·t。
//   q : 实部 = 单位四元数（旋转）
//   t : 对偶部 = ½·v̂ ⊗ q（v̂ = (0, 平移)）。默认 (0,0,0,0) 配上默认 q = identity 即单位元。
struct DualQuat {
    Quaternion<float> q{0.0f, 0.0f, 0.0f, 1.0f};
    Quaternion<float> t{0.0f, 0.0f, 0.0f, 0.0f};
};

// 单位对偶四元数（= 恒等刚体变换）。
inline DualQuat DualQuatIdentity() {
    return DualQuat{};
}

// 由「旋转 + 平移」直接构造：t = ½·v̂ ⊗ q。
// Pre: rot 为单位四元数（调用方保证；本函数不归一化，保持纯构造语义）。
inline DualQuat DualQuatFromRotationTranslation(const Quaternion<float>& rot,
                                                const Vec3<float>& trans) {
    const Quaternion<float> v_hat(trans.x(), trans.y(), trans.z(), 0.0f);  // (0, v) 纯四元数
    const Quaternion<float> half_t = v_hat * rot;                          // v̂ ⊗ q
    DualQuat dq;
    dq.q = rot;
    dq.t = Quaternion<float>(0.5f * half_t.x, 0.5f * half_t.y, 0.5f * half_t.z,
                             0.5f * half_t.w);
    return dq;
}

// 刚体矩阵 → 对偶四元数（本工程烘焙期的唯一入口：肤矩阵是矩阵算出来的）。
//   旋转：左上 3×3 反解四元数（Mat4ToQuaternion，Shepperd）。
//   平移：第 3 列（Mat4TranslationOf）。
// Pre: m 为**刚体**矩阵（正交 3×3 + 平移，无缩放/剪切）——工程链路恒满足（见文件头）。
//   违反（含缩放等）→ LOG(FATAL)：刚体矩阵的集合与单位对偶四元数一一对应，含缩放的矩阵
//   根本没有对应物，静默丢缩放会得到“看着像正常工作”的错误渲染，必须当场暴露。
// 容差 1e-3：链路上有多次 float 乘加，正交性只在浮点精度内成立（实测偏差 ~1e-6）。
inline DualQuat DualQuatFromRigidMatrix(const Mat4& m) {
    const float kEps = 1e-3f;
    // 列主序：列 c 的三个分量 = m[c*4 + 0..2]；列长应为 1、两两正交。
    const Vec3<float> c0(m.m[0], m.m[1], m.m[2]);
    const Vec3<float> c1(m.m[4], m.m[5], m.m[6]);
    const Vec3<float> c2(m.m[8], m.m[9], m.m[10]);
    CHECK(std::fabs(c0.Norm() - 1.0f) < kEps && std::fabs(c1.Norm() - 1.0f) < kEps &&
          std::fabs(c2.Norm() - 1.0f) < kEps)
        << "DualQuatFromRigidMatrix: 3×3 不是正交阵（列长偏离 1，含缩放？）";
    CHECK(std::fabs(c0.Dot(c1)) < kEps && std::fabs(c0.Dot(c2)) < kEps &&
          std::fabs(c1.Dot(c2)) < kEps)
        << "DualQuatFromRigidMatrix: 3×3 不是正交阵（列不正交，含剪切？）";
    CHECK(std::fabs(m.m[3]) < kEps && std::fabs(m.m[7]) < kEps &&
          std::fabs(m.m[11]) < kEps && std::fabs(m.m[15] - 1.0f) < kEps)
        << "DualQuatFromRigidMatrix: 末行须为 [0,0,0,1]（仿射）";

    const Quaternion<float> rot = Mat4ToQuaternion(m);
    return DualQuatFromRotationTranslation(rot, Mat4TranslationOf(m));
}

// 提取平移向量：v = 2·vec(t ⊗ q*)（推导见文件头）。
// Pre: dq 为单位对偶四元数（先 DualQuatNormalized 再调用；否则结果无几何意义）。
inline Vec3<float> DualQuatTranslationOf(const DualQuat& dq) {
    const Quaternion<float> s = dq.t * dq.q.Conjugate();
    return Vec3<float>(2.0f * s.x, 2.0f * s.y, 2.0f * s.z);
}

// 归一化：实部与对偶部**同除 |q|**（只除实部会让平移错，见文件头约束 1）。
// 近零范数（|q| ≤ 1e-8，病态输入）时不除，原样返回 —— 与 Quaternion::NormalizeInPlace
// 同策略（避免 NaN 扩散）；调用方（如 DualQuatBlendWithReference）负责先挡退化情形。
inline DualQuat DualQuatNormalized(const DualQuat& dq) {
    const float n = dq.q.Norm();
    if (n <= 1e-8f) {
        return dq;
    }
    const float inv = 1.0f / n;
    DualQuat r;
    r.q = Quaternion<float>(dq.q.x * inv, dq.q.y * inv, dq.q.z * inv, dq.q.w * inv);
    r.t = Quaternion<float>(dq.t.x * inv, dq.t.y * inv, dq.t.z * inv, dq.t.w * inv);
    return r;
}

// 对偶四元数 → 刚体矩阵（M = T(v)·R(r)）。供单测与 CPU 侧比对用（渲染走 shader 同公式）。
// 语义：先归一化（|q|≠1 时平移也被同除，见文件头约束 1）。
inline Mat4 DualQuatToRigidMatrix(const DualQuat& dq) {
    const DualQuat n = DualQuatNormalized(dq);
    return Mat4Mul(Mat4Translation(DualQuatTranslationOf(n)), Mat4Rotation(n.q));
}

// 用对偶四元数的**旋转部分**旋转方向向量（法线/切线用；平移对方向无影响）。
// Pre: dq 的实部为单位四元数。公式同 geom::RotateVector（此处重述以保持纯旋转语义、
//   避免调用者误以为会带上平移）。
inline Vec3<float> DualQuatRotateVector(const DualQuat& dq, const Vec3<float>& v) {
    const float qx = dq.q.x, qy = dq.q.y, qz = dq.q.z, qw = dq.q.w;
    const float vx = v.x(), vy = v.y(), vz = v.z();
    // cross(q_xyz, v)
    const float cx = qy * vz - qz * vy;
    const float cy = qz * vx - qx * vz;
    const float cz = qx * vy - qy * vx;
    // cross(q_xyz, cross(q_xyz, v))
    const float dx = qy * cz - qz * cy;
    const float dy = qz * cx - qx * cz;
    const float dz = qx * cy - qy * cx;
    // v' = v + 2·qw·(q_xyz × v) + 2·(q_xyz × (q_xyz × v))
    return Vec3<float>(vx + 2.0f * qw * cx + 2.0f * dx,
                       vy + 2.0f * qw * cy + 2.0f * dy,
                       vz + 2.0f * qw * cz + 2.0f * dz);
}

// 变换点：p' = 旋转(p) + 平移（两者都由对偶四元数给出）。
// Pre: dq 为单位对偶四元数（先 DualQuatNormalized）。
inline Vec3<float> DualQuatTransformPoint(const DualQuat& dq, const Vec3<float>& p) {
    const Vec3<float> rotated = DualQuatRotateVector(dq, p);
    const Vec3<float> trans = DualQuatTranslationOf(dq);
    return Vec3<float>(rotated.x() + trans.x(), rotated.y() + trans.y(),
                       rotated.z() + trans.z());
}

// 复合：c = a ⊗ b，语义「先作用 b、再作用 a」（与 Mat4Mul 一致）。
//   实部：ra·rb；对偶部：ra·tb + ta·rb（对偶四元数乘法，ε² = 0）。
// Pre: 两侧均为单位对偶四元数（结果亦为单位）。
inline DualQuat DualQuatMultiply(const DualQuat& a, const DualQuat& b) {
    DualQuat c;
    c.q = a.q * b.q;
    // 对偶部 = ra·tb + ta·rb（逐分量相加：geom::Quaternion 不提供 operator+）。
    const Quaternion<float> rb_tb = a.q * b.t;
    const Quaternion<float> ra_ta = a.t * b.q;
    c.t = Quaternion<float>(rb_tb.x + ra_ta.x, rb_tb.y + ra_ta.y, rb_tb.z + ra_ta.z,
                            rb_tb.w + ra_ta.w);
    return c;
}

// 取负（q 与 t 同时取负 = 同一个刚体变换的另一种表示），抗对偶修正用。
inline DualQuat DualQuatNegated(const DualQuat& dq) {
    DualQuat r;
    r.q = Quaternion<float>(-dq.q.x, -dq.q.y, -dq.q.z, -dq.q.w);
    r.t = Quaternion<float>(-dq.t.x, -dq.t.y, -dq.t.z, -dq.t.w);
    return r;
}

// 两个对偶四元数**旋转部分**的点积（抗对偶判据：< 0 表示处于不同半球）。
inline float DualQuatRotationDot(const DualQuat& a, const DualQuat& b) {
    return a.q.Dot(b.q);
}

// 「同一根骨」在两个姿态间插值：逐分量 lerp + 归一化（NLERP）。
//   先按**最短路径**统一符号（dot(a.q,b.q) < 0 → b 整体取负），否则大角度相邻帧会插到
//   反方向去。ratio ≤ 0 短路返回 a（与渲染端「uRatio ≤ 0 只取 pose_a」逐位一致）；
//   ratio ≥ 1 返回 b。中间值归一化后返回**单位**对偶四元数。
inline DualQuat DualQuatLerp(const DualQuat& a, const DualQuat& b, float ratio) {
    if (ratio <= 0.0f) {
        return a;
    }
    if (ratio >= 1.0f) {
        return b;
    }
    const DualQuat bb = (DualQuatRotationDot(a, b) < 0.0f) ? DualQuatNegated(b) : b;
    DualQuat r;
    r.q = Quaternion<float>(a.q.x + ratio * (bb.q.x - a.q.x),
                            a.q.y + ratio * (bb.q.y - a.q.y),
                            a.q.z + ratio * (bb.q.z - a.q.z),
                            a.q.w + ratio * (bb.q.w - a.q.w));
    r.t = Quaternion<float>(a.t.x + ratio * (bb.t.x - a.t.x),
                            a.t.y + ratio * (bb.t.y - a.t.y),
                            a.t.z + ratio * (bb.t.z - a.t.z),
                            a.t.w + ratio * (bb.t.w - a.t.w));
    return DualQuatNormalized(r);
}

// 跨骨加权混合（DLB）：Σ w_i·q̂_i 后归一化，混合前按**参考骨**统一符号（抗对偶）。
// 渲染端 shader 与测试侧 CPU 真值都按**本函数的顺序与判据**实现（逐像素对齐的第一前提）。
// Pre: count ≥ 1；weights 非负；reference_index ∈ [0, count)。
// 退化处理（与 shader 的写法一一对应，别只改一边）：
//   - Σ weights ≤ 0（顶点没有任何有效权重）→ 返回单位元（= 顶点保持 rest，不塌到原点）。
//   - |Σ w_i·q_i| ≤ 1e-8（权重和近零的病态情形）→ 同样返回单位元。
inline DualQuat DualQuatBlendWithReference(const DualQuat* dqs, const float* weights,
                                          int count, int reference_index) {
    CHECK(dqs != nullptr) << "DualQuatBlendWithReference: dqs 不能为空";
    CHECK(weights != nullptr) << "DualQuatBlendWithReference: weights 不能为空";
    CHECK_GE(count, 1) << "DualQuatBlendWithReference: count 必须 ≥1";
    CHECK_GE(reference_index, 0) << "DualQuatBlendWithReference: reference_index 越界";
    CHECK_LT(reference_index, count) << "DualQuatBlendWithReference: reference_index 越界";

    float weight_sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        weight_sum += weights[i];
    }
    if (weight_sum <= 0.0f) {
        return DualQuatIdentity();
    }

    // 逐分量累加（geom::Quaternion 不提供 operator+，此处显式写，避免为一个用例扩公共类型）。
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f, tw = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (weights[i] <= 0.0f) {
            continue;
        }
        const DualQuat dq = (DualQuatRotationDot(dqs[i], dqs[reference_index]) < 0.0f)
                                ? DualQuatNegated(dqs[i])
                                : dqs[i];
        const float w = weights[i];
        qx += w * dq.q.x;
        qy += w * dq.q.y;
        qz += w * dq.q.z;
        qw += w * dq.q.w;
        tx += w * dq.t.x;
        ty += w * dq.t.y;
        tz += w * dq.t.z;
        tw += w * dq.t.w;
    }
    const float q_norm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (q_norm <= 1e-8f) {
        return DualQuatIdentity();  // 病态：符号统一后仍完全抵消（理论上不该出现）
    }
    DualQuat out;
    out.q = Quaternion<float>(qx, qy, qz, qw);
    out.t = Quaternion<float>(tx, ty, tz, tw);
    return DualQuatNormalized(out);
}

}  // namespace math
}  // namespace geom

#endif  // GEOM_MATH_DUAL_QUAT_H_

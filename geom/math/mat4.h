// Mat4 — 4x4 齐次变换矩阵（列主序 float[16]）与基础运算
//
// 用途：骨架/蒙皮侧需要「构造 rest 变换（平移×旋转）→ 沿树复合 → 求逆（得 inverse bind）」
// 这一串纯 CPU 矩阵运算。此前这些算子散落在 tools/jpov/src/skeleton/skeleton_manager.cc
// 的匿名 namespace 里（不可复用、不可单测）；本文件把它们提到 geom 公共层。
//
// 存储约定（与工程既有约定一致，勿改）：
//   - **列主序**：m[col * 4 + row]。m[12..14] = 平移。（与 object3d / primitives3d 的
//     float[16] 以及 GLSL mat4 的内存布局一致，可直传 glUniformMatrix4fv(transpose=GL_FALSE)。）
//   - 变换作用在点上为左乘：p' = M · p。复合 c = a × b 表示「先作用 b、再作用 a」。
//
// 设计取舍：
//   - 不用 `operator*` / 构造糖，全部具名函数 —— 矩阵乘法不满足交换律，具名比重载更直白，
//     也与工程其它数值代码风格一致。
//   - `Inverse()` 只支持**仿射**变换（旋转/平移/缩放，末行恒为 [0,0,0,1]）；本工程骨架
//     只产生仿射变换。非仿射矩阵 LOG(FATAL) 而非静默返回错值（不 fallback，见工程约定）。
//   - 命名空间 geom::math（与 piecewise_linear_function.h 同层）。

#pragma once

#include <cmath>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"

namespace geom {
namespace math {

// ==================== 基础构造 ====================

// 4x4 矩阵，列主序：m[col * 4 + row]。
struct Mat4 {
    float m[16];
};

// 单位矩阵。
inline Mat4 Mat4Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

// 平移矩阵（列主序，平移落在第 3 列 m[12..14]）。
inline Mat4 Mat4Translation(float x, float y, float z) {
    Mat4 r = Mat4Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline Mat4 Mat4Translation(const Vec3<float>& t) {
    return Mat4Translation(t.x(), t.y(), t.z());
}

// 由四元数构造旋转矩阵（列主序）。pre: 传入四元数须已单位化（调用方保证）。
inline Mat4 Mat4Rotation(const Quaternion<float>& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = Mat4Identity();
    // col0
    r.m[0] = 1.0f - 2.0f * (y * y + z * z);
    r.m[1] = 2.0f * (x * y + z * w);
    r.m[2] = 2.0f * (x * z - y * w);
    // col1
    r.m[4] = 2.0f * (x * y - z * w);
    r.m[5] = 1.0f - 2.0f * (x * x + z * z);
    r.m[6] = 2.0f * (y * z + x * w);
    // col2
    r.m[8]  = 2.0f * (x * z + y * w);
    r.m[9]  = 2.0f * (y * z - x * w);
    r.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

// 由轴 + 角（弧度）构造旋转矩阵。内部归一化轴（零轴则返回单位阵）。
inline Mat4 Mat4RotationAxisAngle(const Vec3<float>& axis, float radians) {
    const float len = axis.Norm();
    if (len <= 1e-8f) {
        return Mat4Identity();
    }
    return Mat4Rotation(Quaternion<float>::FromAxisAngle(axis.Unit(), radians));
}

// ==================== 运算 ====================

// c = a × b。语义：对点先作用 b，再作用 a（列主序下的标准结合）。
inline Mat4 Mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 c{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            c.m[col * 4 + row] = sum;
        }
    }
    return c;
}

// 把点（w=1）喂给矩阵：p' = M · (x,y,z,1)。返回齐次化后的 xyz。
// ⚠️ 仿射矩阵 w' 恒为 1，这里不做除法；透视矩阵不适用（本工程不用）。
inline Vec3<float> Mat4TransformPoint(const Mat4& m, const Vec3<float>& p) {
    const float x = p.x(), y = p.y(), z = p.z();
    return Vec3<float>(m.m[0] * x + m.m[4] * y + m.m[8]  * z + m.m[12],
                       m.m[1] * x + m.m[5] * y + m.m[9]  * z + m.m[13],
                       m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14]);
}

// 把方向（w=0）喂给矩阵：忽略平移分量。
inline Vec3<float> Mat4TransformVector(const Mat4& m, const Vec3<float>& v) {
    const float x = v.x(), y = v.y(), z = v.z();
    return Vec3<float>(m.m[0] * x + m.m[4] * y + m.m[8]  * z,
                       m.m[1] * x + m.m[5] * y + m.m[9]  * z,
                       m.m[2] * x + m.m[6] * y + m.m[10] * z);
}

// 提取平移分量（第 3 列）。
inline Vec3<float> Mat4TranslationOf(const Mat4& m) {
    return Vec3<float>(m.m[12], m.m[13], m.m[14]);
}

// 求逆（**仅仿射**：末行须为 [0,0,0,1]）。
// 仿射矩阵 A = [R|t]，其逆 = [R⁻¹ | -R⁻¹t]；这里对左上 3x3 做通用伴随/行列式求逆，
// 以同时容纳含缩放的仿射（骨架 rest 平移不影响 3x3，但保险起见按通用式写）。
// 非仿射（末行不是 [0,0,0,1]）→ LOG(FATAL)：本工程不产生，静默给错值会埋雷。
inline Mat4 Mat4InverseAffine(const Mat4& m) {
    const float kEpsRow = 1e-5f;
    CHECK(std::fabs(m.m[3]) < kEpsRow && std::fabs(m.m[7]) < kEpsRow &&
          std::fabs(m.m[11]) < kEpsRow && std::fabs(m.m[15] - 1.0f) < kEpsRow)
        << "Mat4InverseAffine: 只支持仿射矩阵（末行须为 [0,0,0,1]）";

    // 左上 3x3（列主序取法：a[col][row] = m[col*4+row]）。
    const float a00 = m.m[0], a10 = m.m[1], a20 = m.m[2];   // col0
    const float a01 = m.m[4], a11 = m.m[5], a21 = m.m[6];   // col1
    const float a02 = m.m[8], a12 = m.m[9], a22 = m.m[10];  // col2

    const float det = a00 * (a11 * a22 - a12 * a21) -
                      a01 * (a10 * a22 - a12 * a20) +
                      a02 * (a10 * a21 - a11 * a20);
    CHECK(std::fabs(det) > 1e-12f)
        << "Mat4InverseAffine: 3x3 部分奇异（det≈0），不可逆";

    const float inv_det = 1.0f / det;
    // 3x3 逆（列主序结果，转置后写回）。
    Mat4 r{};
    r.m[0] = (a11 * a22 - a12 * a21) * inv_det;
    r.m[1] = (a12 * a20 - a10 * a22) * inv_det;
    r.m[2] = (a10 * a21 - a11 * a20) * inv_det;

    r.m[4] = (a02 * a21 - a01 * a22) * inv_det;
    r.m[5] = (a00 * a22 - a02 * a20) * inv_det;
    r.m[6] = (a01 * a20 - a00 * a21) * inv_det;

    r.m[8]  = (a01 * a12 - a02 * a11) * inv_det;
    r.m[9]  = (a02 * a10 - a00 * a12) * inv_det;
    r.m[10] = (a00 * a11 - a01 * a10) * inv_det;

    // 平移列 = -R⁻¹ · t
    const float tx = m.m[12], ty = m.m[13], tz = m.m[14];
    r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8]  * tz);
    r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9]  * tz);
    r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
    r.m[15] = 1.0f;
    return r;
}

// ==================== 骨架常用组合 ====================

// 关节的局部 rest 变换：local = T(rest_offset) × R(bind_rotation)。
// 语义（骨架约定）：**先平移骨长、再按 bind 朝向旋转**；旋转不改变自身位置，只决定朝向。
// 这是「pose 全恒等时 = T-pose」的构造式（identity pose 下 jointLocal 即此式）。
inline Mat4 JointLocalRest(const Vec3<float>& rest_offset,
                           const Quaternion<float>& bind_rotation) {
    return Mat4Mul(Mat4Translation(rest_offset), Mat4Rotation(bind_rotation));
}

// 关节的局部变换：local = T(rest_offset) × R(bind_rotation) × R(pose_rotation)。
// pose_rotation 为恒等时退化为 JointLocalRest（= T-pose）。
inline Mat4 JointLocal(const Vec3<float>& rest_offset,
                       const Quaternion<float>& bind_rotation,
                       const Quaternion<float>& pose_rotation) {
    return Mat4Mul(JointLocalRest(rest_offset, bind_rotation),
                   Mat4Rotation(pose_rotation));
}

}  // namespace math
}  // namespace geom

// geom/common/quaternion.h — 四元数与旋转工具（骨架动画 pose 用）
//
// 用途：JPOV SkeletonPose 用相对父链的四元数表达每骨旋转；CPU 需要做四元数
// 的乘法（链上组合）、归一化、用四元数旋转一个向量、在姿态间 slerp 插值。
// 本文件提供一套最小且自洽的 `geom::Quaternion<T>`（T 典型 float），以吻合
// geom::Vec<T,Dim> 的模板/裸数组血统（见同目录 vec.h：T m[Dim]）。
//
// 设计取舍（严谨落点，2026-09-08 详读 geom 后定）：
//   - 放 geom/common 与 vec 平级（同是几何/线性基础原语），namespace geom；
//     NOT geom/math（那放“函数/曲线”如 piecewise）。
//   - 模板化 T（像 vec），JPOV 侧用别名 Quaternionf = Quaternion<float>。
//   - 只给骨架/动画真正需要的最小算子；勿贪多 copy 整个线性代数库。
//   - w 分量定义：Hamilton 约定 xyzw = (x,y,z,w)，Q = w + x i + y j + z k。
//   - 左乘语义：rotation(q1) ∘ rotation(q2) == q1*q2（先 q2 后 q1，imagine
//     《链上组合父在前/子在后需一致》，见下 Multiply）。
//   - 不与 glm 绑定：只看 Vec3 rotate。
//
// 依赖：仅 geom/common 的 vec.h（rotate 需要 Vec3）。不必引入矩阵库。

#ifndef GEOM_COMMON_QUATERNION_H_
#define GEOM_COMMON_QUATERNION_H_

#include <cmath>
#include <type_traits>

#include "geom/common/vec.h"

namespace geom {

// 四元数 rotation 类型（模板化）。字段 x,y,z,w（Hamilton），T 通常 float。
template <typename T>
struct Quaternion {
    static_assert(std::is_floating_point<T>::value,
                  "Quaternion scalar must be floating point");

    T x{0}, y{0}, z{0}, w{1};  // 默认为单位元（identity rotation）

    constexpr Quaternion() = default;

    // 从 (x,y,z,w) Hamilton 分量直接构造。调用方需自行归一化（见 Normalized）。
    constexpr Quaternion(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}

    // ---- 工具 ----
    static Quaternion Identity() { return Quaternion(T{0}, T{0}, T{0}, T{1}); }

    // 范数平方 |q|^2 = x²+y²+z²+w²。
    T NormSqr() const { return x * x + y * y + z * z + w * w; }
    T Norm() const { return std::sqrt(NormSqr()); }

    // 归一化（若近零范数则不除，保持原值，避免 NaN——调用方应避免给零四元数）。
    void NormalizeInPlace() {
        const T n = Norm();
        const T kEps = static_cast<T>(1e-8);
        if (n > kEps) {
            const T inv = T{1} / n;
            x *= inv; y *= inv; z *= inv; w *= inv;
        }
    }
    Quaternion Normalized() const {
        Quaternion r = *this;
        r.NormalizeInPlace();
        return r;
    }

    // 共轭 q* = (-x,-y,-z,w)。用于逆（单位四元数 q^-1 = q*）。
    Quaternion Conjugate() const { return Quaternion(-x, -y, -z, w); }

    // ---- 运算 ----
    // Hamilton 乘法：先 b、后 a（组合旋转 a 再 b“结果 = a*b”？文档：
    //   人们常说 q1*q2 “先应用 q2、后 q1”。本实现严格按 Hamilton，
    //   组合顺序由调用方厘清（骨架根往子链通常 local 乘用 parent 在前）。
    Quaternion operator*(const Quaternion& o) const {
        return Quaternion(
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z);
    }
    Quaternion& operator*=(const Quaternion& o) {
        *this = *this * o;  // 见 operator* 顺序语义
        return *this;
    }

    // 两个四元数点积（求角度/cos 用，非旋转）。
    T Dot(const Quaternion& o) const {
        return x * o.x + y * o.y + z * o.z + w * o.w;
    }

    // 绕单位方向轴 axis 旋转 angle(rad) 的四元数（用于极轴表达/构造）。
    // Pre: axis 已归一化（视长度，内部不再归一：调用方保证，避免歧义）。
    static Quaternion FromAxisAngle(const geom::Vec3<T>& axis, T angle) {
        const T s = std::sin(angle * T{0.5});
        const T c = std::cos(angle * T{0.5});
        // Vec3 访问是方法 x()/y()/z()（见 vec.h）。
        return Quaternion(axis.x() * s, axis.y() * s, axis.z() * s, c);
    }
};

// 用四元数 q 旋转向量 v（单位/归一化的 q 才正确；内部乘左=旋转约定）。
template <typename T>
geom::Vec3<T> RotateVector(const Quaternion<T>& q, const geom::Vec3<T>& v) {
    // 标准公式：v' = v + 2*w*(q_xyz × v) + 2*(q_xyz × (q_xyz × v))
    // (适用于近似单位 q；要更稳可先用 q 归一。)
    const T qx{q.x}, qy{q.y}, qz{q.z}, qw{q.w};
    const T vx{v.x()}, vy{v.y()}, vz{v.z()};
    const T two{2};
    // cross(q_xyz, v)
    const T cx0 = qy * vz - qz * vy;
    const T cy0 = qz * vx - qx * vz;
    const T cz0 = qx * vy - qy * vx;
    // cross(q_xyz, (q_xyz × v))
    const T dx = qy * cz0 - qz * cy0;
    const T dy = qz * cx0 - qx * cz0;
    const T dz = qx * cy0 - qy * cx0;
    // v' = v + 2*qw*(r×v) + 2*(q_xyz×(q_xyz×v))
    return geom::Vec3<T>(
        vx + two * qw * cx0 + two * dx,
        vy + two * qw * cy0 + two * dy,
        vz + two * qw * cz0 + two * dz);
}

// 两个单位四元数之间的球面线性插值（动画帧间姿态过渡需要）。
// a、b 需为单位四元数。t∈[0,1]。若 a、b 朝向相反(dot<0)会翻转：先取最短路径符号。
template <typename T>
Quaternion<T> Slerp(const Quaternion<T>& a, const Quaternion<T>& b, T t) {
    T dot = a.Dot(b);
    Quaternion<T> b2 = b;
    // 取最短（若 dot 为负翻转 b，避免绕远路/不自然翻转）。
    if (dot < T{0}) {
        b2 = Quaternion<T>(-b.x, -b.y, -b.z, -b.w);
        dot = -dot;
    }
    const T kAlign = static_cast<T>(1 - 1e-4);
    if (dot > kAlign) {
        // 几乎平行 → 线性插值 + 归一更快更稳。
        Quaternion<T> r(
            a.x + t * (b2.x - a.x),
            a.y + t * (b2.y - a.y),
            a.z + t * (b2.z - a.z),
            a.w + t * (b2.w - a.w));
        r.NormalizeInPlace();
        return r;
    }
    // 真球面。
    dot = std::max(T{-1}, std::min(T{1}, dot));
    const T theta = std::acos(dot);
    const T sinTheta = std::sin(theta);
    if (sinTheta < static_cast<T>(1e-8)) return a;  // 防御（几乎同向）
    const T o = std::sin((T{1} - t) * theta) / sinTheta;
    const T i = std::sin(t * theta) / sinTheta;
    return Quaternion<T>(
        o * a.x + i * b2.x,
        o * a.y + i * b2.y,
        o * a.z + i * b2.z,
        o * a.w + i * b2.w);
}

}  // namespace geom

#endif  // GEOM_COMMON_QUATERNION_H_

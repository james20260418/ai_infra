// JPOV 局部体积雾 — 闭式积分核（GL-free，纯 CPU，可单测）
//
// 见 docs/jpov_volumetric_fog_design.md §6。核心：密度剖面写成「距离平方的多项式」，
// 沿视线弦 p(t)=o+t·d（d 单位）被积函数是 t 的多项式 ⇒ 解析积分 τ=∫σ dt。
// 深度裁剪 = 把积分端点在 [t_min, t_max] 内夹断（多项式定积分端点任意，裁剪仍闭式）。
//
// 与 GPU 侧实现（fog_pass_shader.h 的 GLSL）**逐式对应**；此处用 double 做参考/单测，
// GPU 用 float（llvmpipe 下确定）。
//
// 约定：o/d 为世界坐标、d 为单位向量；t 的量纲 = 米；τ 无量纲；σ₀ = FogXxx::intensity。

#ifndef JPOV_SRC_VOLUMETRIC_FOG_FOG_VOLUME_H_
#define JPOV_SRC_VOLUMETRIC_FOG_FOG_VOLUME_H_

#include <cmath>

#include "tools/jpov/interface/fog_volume.h"

namespace jpov {
namespace volumetric_fog {

// 次数 ≤ 6 的多项式：p(t) = c[0] + c[1] t + … + c[6] t^6。
struct Poly {
    double c[7];

    static Poly Constant(double v) {
        Poly p{{0, 0, 0, 0, 0, 0, 0}};
        p.c[0] = v;
        return p;
    }
};

inline Poly PolyAdd(const Poly& a, const Poly& b) {
    Poly r{{0, 0, 0, 0, 0, 0, 0}};
    for (int i = 0; i < 7; ++i) {
        r.c[i] = a.c[i] + b.c[i];
    }
    return r;
}

inline Poly PolyScale(const Poly& a, double s) {
    Poly r{{0, 0, 0, 0, 0, 0, 0}};
    for (int i = 0; i < 7; ++i) {
        r.c[i] = a.c[i] * s;
    }
    return r;
}

// 多项式乘（结果截断到 ≤6 次；本场景最高正好 6 次，无有效项丢失）。
inline Poly PolyMul(const Poly& a, const Poly& b) {
    Poly r{{0, 0, 0, 0, 0, 0, 0}};
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7 - i; ++j) {
            r.c[i + j] += a.c[i] * b.c[j];
        }
    }
    return r;
}

inline double PolyEval(const Poly& p, double t) {
    double v = 0.0;
    for (int i = 6; i >= 0; --i) {
        v = v * t + p.c[i];
    }
    return v;
}

// 定积分 ∫_a^b p(t) dt。a > b 时返回负值（调用方保证 a ≤ b）。
inline double PolyIntegrate(const Poly& p, double a, double b) {
    double ia = 0.0;
    double ib = 0.0;
    double pa = 1.0;
    double pb = 1.0;
    for (int k = 0; k < 7; ++k) {
        pa *= a;  // a^(k+1)
        pb *= b;
        ia += p.c[k] * pa / static_cast<double>(k + 1);
        ib += p.c[k] * pb / static_cast<double>(k + 1);
    }
    return ib - ia;
}

// ---------- 球雾 ----------

// 球雾 τ（闭式；端点夹在 [t_min, t_max]）。d 必须为单位向量。
// Pre-condition: radius > 0, intensity ≥ 0
inline double SphereTau(const FogSphere& f, const double o[3],
                        const double d[3], double t_min, double t_max) {
    if (f.radius <= 0.0f || f.intensity <= 0.0f) {
        return 0.0;
    }
    const double r2 = static_cast<double>(f.radius) * f.radius;
    const double mx = o[0] - f.center.x();
    const double my = o[1] - f.center.y();
    const double mz = o[2] - f.center.z();
    const double b = mx * d[0] + my * d[1] + mz * d[2];
    const double mm = mx * mx + my * my + mz * mz;
    const double cc = mm - r2;
    const double disc = b * b - cc;
    if (disc <= 0.0) {
        return 0.0;  // 视线与球不相交
    }
    const double half = std::sqrt(disc);   // 半弦长 L
    const double t0 = -b - half;
    const double t1 = -b + half;
    const double lo = std::max(t0, t_min);
    const double hi = std::min(t1, t_max);
    if (hi <= lo) {
        return 0.0;
    }
    const double tc = -b;                  // 垂足参数
    const double h2 = mm - b * b;          // 垂距平方
    const double a = 1.0 / r2;
    const double beta = 1.0 - a * h2;      // 垂足处密度系数（≤1）
    // q(t) = beta - a (t - tc)^2 = (beta - a tc^2) + (2 a tc) t + (-a) t^2
    Poly q{{0, 0, 0, 0, 0, 0, 0}};
    q.c[0] = beta - a * tc * tc;
    q.c[1] = 2.0 * a * tc;
    q.c[2] = -a;
    Poly dens = (f.profile == FogProfile::kSharp) ? PolyMul(q, q) : q;
    return static_cast<double>(f.intensity) * PolyIntegrate(dens, lo, hi);
}

// ---------- 圆柱雾 ----------

// 圆柱雾 τ（径向 × 轴向分离式闭式；端点夹在 [t_min, t_max]）。d 必须为单位向量。
// Pre-condition: radius > 0, height > 0, intensity ≥ 0, axis 非零
inline double CylinderTau(const FogCylinder& f, const double o[3],
                          const double d[3], double t_min, double t_max) {
    constexpr double kEps = 1e-12;
    if (f.radius <= 0.0f || f.height <= 0.0f || f.intensity <= 0.0f) {
        return 0.0;
    }
    double ax = f.axis.x();
    double ay = f.axis.y();
    double az = f.axis.z();
    const double alen = std::sqrt(ax * ax + ay * ay + az * az);
    if (alen < kEps) {
        return 0.0;
    }
    ax /= alen; ay /= alen; az /= alen;

    const double ux = o[0] - f.base.x();
    const double uy = o[1] - f.base.y();
    const double uz = o[2] - f.base.z();
    const double d_ax = d[0] * ax + d[1] * ay + d[2] * az;   // d·axis
    const double y0 = ux * ax + uy * ay + uz * az;           // (o-base)·axis
    const double u_d = ux * d[0] + uy * d[1] + uz * d[2];    // (o-base)·d
    const double uu = ux * ux + uy * uy + uz * uz;

    // ρ²(t) = A t² + B t + C（ρ = 到轴的横距）
    const double A = 1.0 - d_ax * d_ax;
    const double B = 2.0 * (u_d - y0 * d_ax);
    const double C = uu - y0 * y0;
    const double R2 = static_cast<double>(f.radius) * f.radius;

    // 径向支撑：ρ² ≤ R²
    double rad_lo = -1e30, rad_hi = 1e30;
    if (A > kEps) {
        const double disc = B * B - 4.0 * A * (C - R2);
        if (disc < 0.0) {
            return 0.0;  // 横截面不相交
        }
        const double s = std::sqrt(disc);
        const double r1 = (-B - s) / (2.0 * A);
        const double r2 = (-B + s) / (2.0 * A);
        rad_lo = std::min(r1, r2);
        rad_hi = std::max(r1, r2);
    } else {
        // 视线几乎平行于轴：ρ² ≈ B t + C
        if (std::fabs(B) < kEps) {
            if (C > R2) {
                return 0.0;
            }
        } else {
            const double root = (R2 - C) / B;
            if (B > 0.0) {
                rad_hi = root;
            } else {
                rad_lo = root;
            }
        }
    }

    // 轴向支撑：0 ≤ y(t) ≤ h
    const double hgt = static_cast<double>(f.height);
    double z_lo = -1e30, z_hi = 1e30;
    if (std::fabs(d_ax) > kEps) {
        const double ta = (0.0 - y0) / d_ax;
        const double tb = (hgt - y0) / d_ax;
        z_lo = std::min(ta, tb);
        z_hi = std::max(ta, tb);
    } else if (y0 < 0.0 || y0 > hgt) {
        return 0.0;
    }

    const double lo = std::max(std::max(rad_lo, z_lo), t_min);
    const double hi = std::min(std::min(rad_hi, z_hi), t_max);
    if (hi <= lo) {
        return 0.0;
    }

    // f_r(t) = 1 - ρ²/R² = (1 - C/R2) - (B/R2) t - (A/R2) t²
    Poly fr{{0, 0, 0, 0, 0, 0, 0}};
    fr.c[0] = 1.0 - C / R2;
    fr.c[1] = -B / R2;
    fr.c[2] = -A / R2;
    if (f.radial_profile == FogProfile::kSharp) {
        fr = PolyMul(fr, fr);
    }

    // f_y(t)：kUniform=1；kDomeAxial=1-(y/h)²
    Poly fy = Poly::Constant(1.0);
    if (f.axial_profile == FogProfile::kDomeAxial) {
        const double h2 = hgt * hgt;
        fy.c[0] = 1.0 - (y0 * y0) / h2;
        fy.c[1] = -2.0 * y0 * d_ax / h2;
        fy.c[2] = -(d_ax * d_ax) / h2;
    }

    const Poly dens = PolyMul(fr, fy);
    return static_cast<double>(f.intensity) * PolyIntegrate(dens, lo, hi);
}

// 数值参考（Simpson），仅供单测交叉验证，不用于渲染。
inline double SphereTauReference(const FogSphere& f, const double o[3],
                                 const double d[3], double t_min,
                                 double t_max, int n) {
    const double r2 = static_cast<double>(f.radius) * f.radius;
    const double mx = o[0] - f.center.x();
    const double my = o[1] - f.center.y();
    const double mz = o[2] - f.center.z();
    const double b = mx * d[0] + my * d[1] + mz * d[2];
    const double cc = mx * mx + my * my + mz * mz - r2;
    const double disc = b * b - cc;
    if (disc <= 0.0) return 0.0;
    const double half = std::sqrt(disc);
    double lo = std::max(-b - half, t_min);
    double hi = std::min(-b + half, t_max);
    if (hi <= lo) return 0.0;
    if (n < 2) n = 2;
    if (n % 2 == 1) ++n;
    const double step = (hi - lo) / n;
    double sum = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double t = lo + step * i;
        const double px = mx + t * d[0];
        const double py = my + t * d[1];
        const double pz = mz + t * d[2];
        const double u2 = (px * px + py * py + pz * pz) / r2;
        double dens = 1.0 - u2;
        if (dens < 0.0) dens = 0.0;
        if (f.profile == FogProfile::kSharp) dens *= dens;
        const double w = (i == 0 || i == n) ? 1.0 : ((i % 2) ? 4.0 : 2.0);
        sum += w * dens;
    }
    return static_cast<double>(f.intensity) * sum * step / 3.0;
}

}  // namespace volumetric_fog
}  // namespace jpov

#endif  // JPOV_SRC_VOLUMETRIC_FOG_FOG_VOLUME_H_

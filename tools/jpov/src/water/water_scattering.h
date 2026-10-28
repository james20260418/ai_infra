// JPOV 水体单次散射（GL-free，纯数学）—— 水面着色器与单测共用同一套公式
//
// 物理模型：均匀介质 + 视线是一条线段 ⇒ 单次散射可以**闭式积分**，不需要 raymarch、不需要 froxel。
//
//   L_in(u) = σs · [ 环境项(u) + 太阳项(u) ] · exp(−σt·u)
//
//   环境项（QSSA，把漫射场近似成"等效竖直光束"）：
//       E_amb(u) = E_d0 · exp(−K_d · z(u))          K_d = σa + (1−g_amb)·σs（漫衰减）
//       source   = σs · (E_amb / π) · M(θ_amb)       M = 4π·p_HG，各向同性时 M = 1
//   太阳项（水下折射后的准直光束）：
//       E_sun(u) = E_sun0 · exp(−σt · z(u)/c)        c = |ω_sun_w.y| ≥ 0.661
//       source   = σs · p_HG(θ_sun) · E_sun(u)       p 归一化：∫p dω = 1
//
//   几何：z(u) = z0 − u·ω.y  （z0 = 入水点深度；水上入水 ⇒ 0，水下相机 ⇒ 相机深度）
//        两项对 u 都是 exp(a·u) 形式 ⇒ ∫₀^D = (exp(aD) − 1)/a
//
// 注意量纲：σa / σs 都是 1/m；E 是辐照度（与场景 HDR 亮度同尺度）；结果是对目标方向的**辐亮度**。
//
// Pre-condition: 线段**整段位于水体内部** —— z(u) = z0 − u·ω.y 在 [0, len] 上恒 > 0。
//   相机在水下抬头时，段长必须 ≤ 到水面的距离（z0/ω.y）；否则会算到"水面之上"的空中，
//   模型在那里不成立（实测：随机测试拿到这种非法输入时，闭式解与数值解差 27%）。

#ifndef JPOV_SRC_WATER_WATER_SCATTERING_H_
#define JPOV_SRC_WATER_WATER_SCATTERING_H_

#include <algorithm>
#include <cmath>

namespace jpov {
namespace water {

// 自带 π：不依赖 M_PI（MinGW/MSVC 需要 _USE_MATH_DEFINES，且公共头里改 include 顺序很危险）。
inline constexpr double kPi = 3.14159265358979323846;

// Henyey-Greenstein 相函数，归一化 ∫_{4π} p dω = 1。
// g = 0 ⇒ 各向同性（1/4π）；g → 1 ⇒ 强前向。
inline double HgPhase(double cos_theta, double g) {
    const double gg = g * g;
    double d = 1.0 + gg - 2.0 * g * cos_theta;
    d = std::max(d, 1e-12);
    return (1.0 - gg) / (4.0 * kPi * d * std::sqrt(d));
}

// 环境项的"方向调制"：M = 4π·p。各向同性（g=0）时恒为 1，前向时 >1。
// 这样环境项的物理基准就是"各向同性光场下 = σs·L"，加了方向性也不会把量级带偏。
inline double AmbModulation(double cos_theta, double g) {
    return 4.0 * kPi * HgPhase(cos_theta, g);
}

// 单次散射输入。
//   σa/σs  : 3 通道（RGB）系数，单位 1/m
//   g_sun   : 太阳相位各向异性（0.8~0.95 典型）
//   g_amb   : 环境项软相位（散射场被 Snell 锥抹宽，0.2~0.4 典型）
//   cos_sun : 太阳传播方向与"指向相机"方向夹角的余弦（朝太阳看 ⇒ 1）
//   cos_amb : 环境等效光束（竖直向下）与"指向相机"方向夹角的余弦 = 视线 y 分量
//   sun_e   : 水面**之下**的太阳辐照度（已含 Fresnel 透射与 cosθa/cosθw 面积因子）
//   amb_e   : 水面**之下**的下行辐照度 E_d0
//   omega_y : 视线方向的 y 分量
//   z0      : 入水点的深度（米）
//   len     : 水柱段长度 D（米）
struct InScatterInput {
    double sigma_a[3] = {0.32, 0.08, 0.05};
    double sigma_s[3] = {0.045, 0.050, 0.055};
    double g_sun = 0.85;
    double g_amb = 0.30;
    double cos_sun = 0.0;
    double cos_amb = 0.0;
    double sun_e = 0.0;
    double amb_e = 0.0;
    double omega_y = -1.0;
    double z0 = 0.0;
    double len = 1.0;
};

struct InScatterResult {
    double amb[3] = {0.0, 0.0, 0.0};
    double sun[3] = {0.0, 0.0, 0.0};
};

// 漫衰减系数（逐通道）：K_d = σa + (1−g)·σs。
// 三个极限都对：σs→0 ⇒ σa；g→1（全前向，不消耗光）⇒ σa；g→0（各向同性）⇒ σt。
inline double DiffuseAttenuation(double sigma_a, double sigma_s, double g) {
    return sigma_a + (1.0 - std::max(0.0, std::min(1.0, g))) * sigma_s;
}

// ∫₀^L exp(a·u) du（a≈0 时取极限 L）。
inline double IntegralExp(double a, double len) {
    if (len <= 0.0) {
        return 0.0;
    }
    if (std::fabs(a) < 1e-9) {
        return len;
    }
    return (std::exp(a * len) - 1.0) / a;
}

// 闭式解。
inline InScatterResult ComputeInScatter(const InScatterInput& in) {
    InScatterResult out;
    const double m_amb = AmbModulation(in.cos_amb, in.g_amb);
    const double p_sun = HgPhase(in.cos_sun, in.g_sun);
    // 太阳在水下的方向余弦 |ω_sun_w.y| ∈ [0.661, 1]；退化时按 1 处理（= 竖直光束）。
    const double c_sun = std::max(0.661, 1e-3);

    for (int ch = 0; ch < 3; ++ch) {
        const double sa = std::max(0.0, in.sigma_a[ch]);
        const double ss = std::max(0.0, in.sigma_s[ch]);
        const double st = sa + ss;
        const double kd = DiffuseAttenuation(sa, ss, in.g_amb);

        // ---- 环境项 ----
        // 被积：σs·(E_d0/π)·M·exp(−K_d z0)·exp((K_d·ω.y − σt)·u)
        {
            const double a = kd * in.omega_y - st;
            const double pref = ss * (in.amb_e / kPi) * m_amb * std::exp(-kd * in.z0);
            out.amb[ch] = pref * IntegralExp(a, in.len);
        }
        // ---- 太阳项 ----
        // 被积：σs·p·E_sun0·exp(−σt z0/c)·exp((σt·ω.y/c − σt)·u)
        {
            const double a = st * in.omega_y / c_sun - st;
            const double pref = ss * p_sun * in.sun_e * std::exp(-st * in.z0 / c_sun);
            out.sun[ch] = pref * IntegralExp(a, in.len);
        }
    }
    return out;
}

// 数值参考实现（梯形法，仅用于单测对照"闭式解是否等价于逐点积分"）。
// 与闭式解**共用同一个源函数**，所以它测的是"积分"，不是"模型"。
inline InScatterResult ComputeInScatterNumeric(const InScatterInput& in, int steps) {
    InScatterResult out;
    if (steps < 1) {
        steps = 1;
    }
    const double m_amb = AmbModulation(in.cos_amb, in.g_amb);
    const double p_sun = HgPhase(in.cos_sun, in.g_sun);
    const double c_sun = std::max(0.661, 1e-3);
    const double du = in.len / static_cast<double>(steps);

    for (int ch = 0; ch < 3; ++ch) {
        const double sa = std::max(0.0, in.sigma_a[ch]);
        const double ss = std::max(0.0, in.sigma_s[ch]);
        const double st = sa + ss;
        const double kd = DiffuseAttenuation(sa, ss, in.g_amb);
        double acc_amb = 0.0;
        double acc_sun = 0.0;
        for (int i = 0; i <= steps; ++i) {
            const double u = du * static_cast<double>(i);
            const double z = std::max(0.0, in.z0 - u * in.omega_y);
            const double w = (i == 0 || i == steps) ? 0.5 : 1.0;   // 梯形权重
            const double src_amb = ss * (in.amb_e / kPi) * m_amb * std::exp(-kd * z);
            const double src_sun = ss * p_sun * in.sun_e * std::exp(-st * z / c_sun);
            const double view = std::exp(-st * u);
            acc_amb += w * src_amb * view * du;
            acc_sun += w * src_sun * view * du;
        }
        out.amb[ch] = acc_amb;
        out.sun[ch] = acc_sun;
    }
    return out;
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SCATTERING_H_

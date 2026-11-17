// JPOV 水体介质（water medium）—— GL-free 的 CPU 参考模型
//
// 定位：水面 `y = level_y` **以下**的半空间里充满均质介质（σa / σs / 相位 g）。
// 本文件 = **介质**：视线在水里的吸收与注入（连续体积，一段）。
// 公式原语（菲涅尔 / 临界角 / 折射 / 相位 / 段积分）不在这里 —— 它们在
// `water_optics.h`，那份是本工程唯一的物理出处。
//
// 为什么是**解析闭式**（2026-11-15 Danis 定，别改成网格）：
//   均质 + 半空间 ⇒ 视线在水里那段长度对参数线性、太阳衰减对深度指数 ⇒ 两个指数合并成
//   一个能直接积出来的式子。没有需要摊销的 3D 场，所以不需要 froxel 的屏幕空间网格
//   （froxel = 牺牲屏幕分辨率换 z 轴并行化数值积分；水没有这个必要）。
//   ⚠️ 前提是「σ 沿视线为常数 + 定义域是平面」。一旦水变成体素/任意形状，这一整套闭式
//   作废，应换成逐像素 DDA（段数少）或 froxel（段数多）—— 见 known_issue §10.5。
//
// 本版**不做**（写在前面，免得当成 bug）：
//   · CSM 阴影：太阳项按「全受照」积分（无 god ray、无岛影）。见 known_issue §10.4；
//   · 多次散射：单次散射 + 常量 ambient ⇒ 会偏暗、对比偏高；
//   · 折射：视线在水里不偏折（表观深度仍是几何深度，物理上应为 d/n ≈ 0.75·d）；
//     **太阳方向**仍然按 Snell 折射（这一项与水面的法线有关，不影响视线几何）；
//   · 环境项按常量 L_amb 积分，不考虑 ambient 场自身沿深度衰减。
//
// 与着色器同源：src/water/water_medium_shader.h 的 `MediumAt` 是同一套公式的 GLSL 版，
//  改一处必须改另一处；test/water:water_medium_gpu_test 用本文件逐像素对拍兜底。

#ifndef JPOV_SRC_WATER_WATER_MEDIUM_MATH_H_
#define JPOV_SRC_WATER_WATER_MEDIUM_MATH_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/interface/camera.h"          // Vec3f（header-only，无 GL）
#include "tools/jpov/src/water/water_optics.h"     // 菲涅尔 / 折射 / 相位 / 段积分

namespace jpov {
namespace water {

// 一帧的介质输入：由 RenderCommandList 解析出的**最小集合**（合成趟只需要这些）。
// 所有颜色量都是「辐亮度/辐照度」语义的 HDR 值，已经乘过各自的 intensity。
struct WaterMediumSample {
    float level_y = 0.0f;                 // 水面世界高度（米，y-up）
    float ior = kWaterIorDefault;         // 界面折射率（只影响菲涅尔/折射，不产生位移）
    Vec3f sigma_a = {0.0f, 0.0f, 0.0f};   // 吸收系数 (1/m)，逐通道，>= 0
    Vec3f sigma_s = {0.0f, 0.0f, 0.0f};   // 散射系数 (1/m)，逐通道，>= 0
    float phase_g = 0.0f;                 // HG 各向异性 ∈ [0, 0.95)
    Vec3f ambient = {0.0f, 0.0f, 0.0f};   // 环境入射（= cmds.ambient，已乘 intensity；
                                           //   三色已按球面权重塔成纯色；未配则为零 = 黑）
    Vec3f sun_color = {0.0f, 0.0f, 0.0f}; // 空气侧太阳（已乘 intensity）
    Vec3f sun_toward = {0.0f, 1.0f, 0.0f};// 指向太阳（空气侧，单位）
    bool has_sun = false;
};

// 从相机沿 rd 走 z 米（**欧氏距离**，与合成趟的 z 同口径），在水体里得到的累积量：
//   tau  —— 视线在水里的路径长度 × σt（逐通道）⇒ 背景透射率 = exp(−tau)；
//   S    —— 单次散射（太阳，全受照）+ 环境项的累积辐亮度（逐通道）。
// 语义与 FogField 的累积 (τ, S) 一致（都是「相机 → 该深度」的前缀），所以两者可以直接相加。
//
// Pre-condition: rd 为单位向量；z >= 0；sigma_a/sigma_s 各分量 >= 0。
// 返回空（0,0）的情况：整段都在水面之上 / 视线不进水 / z <= 0。
inline void WaterAt(const WaterMediumSample& m, const Vec3f& ro, const Vec3f& rd,
                    float z, Vec3f* tau /*output*/, Vec3f* S /*output*/) {
    if (tau == nullptr || S == nullptr) {
        return;
    }
    *tau = {0.0f, 0.0f, 0.0f};
    *S = {0.0f, 0.0f, 0.0f};
    if (!(z > 0.0f)) {
        return;
    }

    // ── 段：射线与半空间 (y <= level) 的交，截到 [0, z] ──
    float t0 = 0.0f;
    float t1 = z;
    if (ro.y() > m.level_y) {
        // 相机在水上：必须先向下穿过水面
        if (rd.y() >= -1.0e-6f) {
            return;   // 向上/平行：永远不进水体
        }
        t0 = (m.level_y - ro.y()) / rd.y();
        if (t0 >= t1) {
            return;   // 水面在物体之后（像素看的是水面上方的东西）
        }
    } else if (rd.y() > 1.0e-6f) {
        // 相机在水下、视线向上：水体到水面为止（再往上就是空气）
        t1 = std::min(t1, (m.level_y - ro.y()) / rd.y());
    }
    const float len = t1 - t0;
    if (!(len > 0.0f)) {
        return;
    }

    // ── 视线在水里的**真实方向**（2026-11-15 L0）──
    // 相机在水上：光线在界面折射后才进水，θw = asin(sinθi/n) ⇒ 竖直方向余弦 dy_w = −cosθw；
    // 相机在水下：水内就是原方向（折射只发生在**出水**那一步，与水段无关）。
    // 水段的真实长度 = 竖直跨度 / |dy_w|；直走参数下的长度 len 要乘 |rd.y|/|dy_w|。
    //   ⚠️ 只修正**路径长度**，不位移图像（后者要靠屏幕空间折射 gather，未做，见 §10.3）。
    const bool from_air = (ro.y() > m.level_y);
    float dy_w = rd.y();
    if (from_air) {
        const float sin2_i = std::max(0.0f, 1.0f - rd.y() * rd.y());
        const float sin2_w = sin2_i / (m.ior * m.ior);
        dy_w = -std::sqrt(std::max(0.0f, 1.0f - sin2_w));
    }
    if (std::fabs(dy_w) < 1.0e-6f) {
        return;
    }
    const float len_w = len * (std::fabs(rd.y()) / std::fabs(dy_w));

    const float st_r = m.sigma_a.x() + m.sigma_s.x();
    const float st_g = m.sigma_a.y() + m.sigma_s.y();
    const float st_b = m.sigma_a.z() + m.sigma_s.z();
    tau->x() = st_r * len_w;
    tau->y() = st_g * len_w;
    tau->z() = st_b * len_w;

    // ── 直射项（闭式）：∫ exp(−σt·[(t−t0) + d(t)/u_w.y]) dt ──
    //   d(t) = level − p(t).y 是「该点到水面」的深度，沿折射后的太阳方向上去的路程 = d/u_w.y。
    //   指数对 t 线性（斜率 σt·B，B 与通道无关）⇒ 可直接积。
    Vec3f toward;
    if (m.has_sun && RefractTowardSun(m.sun_toward, m.ior, &toward)) {
        const float cos_i = m.sun_toward.y();             // 入射角（法线 = +Y）
        // 界面透射：进散射源的是**法向辐照度** E⊥（不是辐射度），故按**功率**分账：
        //   同一块界面上的入射功率 = E⊥,air·cosθa，反射掉 R 后剩 (1−R)·E⊥,air·cosθa；
        //   进水后的“垂直截面”变成 cosθw（光束被折弯）⇒ E⊥,water = (1−R)·cosθa/cosθw。
        // ⇒ gain = (1−R)·cosθa/cosθw（**无 n²**；n² 只属于辐亮度口径）。
        //
        // ✅ 2026-11-17 Danis 拍板修正（原为 (1−R)/n²）：垂直太阳下差 1.777×（原来偏暗），
        //    低太阳时还多一个 cos 比（原来低估倾斜入射）。推导见契约 §5 / known_issue §10.2b。
        const float fresnel = FresnelDielectric(cos_i, 1.0f, m.ior);
        const float gain = (1.0f - fresnel) * (cos_i / toward.y());
        // 真实路径上的指数斜率：视线衰减 1、太阳衰减 (−dy_w)/u_y ⇒ B = 1 − dy_w/u_y。
        const float B = 1.0f - dy_w / toward.y();
        const float d0 = m.level_y - (ro.y() + rd.y() * t0);
        const float cos_theta = std::clamp(rd.Dot(toward), -1.0f, 1.0f);
        const float phase = HgPhase(cos_theta, m.phase_g);
        const float sun_gain = phase * gain;              // 标量，三通道共用

        const float exp_sun_r = std::exp(-st_r * d0 / toward.y());
        const float exp_sun_g = std::exp(-st_g * d0 / toward.y());
        const float exp_sun_b = std::exp(-st_b * d0 / toward.y());
        S->x() = m.sigma_s.x() * m.sun_color.x() * sun_gain * exp_sun_r *
                 DecayLength(st_r, B, len_w);
        S->y() = m.sigma_s.y() * m.sun_color.y() * sun_gain * exp_sun_g *
                 DecayLength(st_g, B, len_w);
        S->z() = m.sigma_s.z() * m.sun_color.z() * sun_gain * exp_sun_b *
                 DecayLength(st_b, B, len_w);
    }

    // ── 环境项（各向同性，无相位、无方向衰减）──
    //   只有视线方向的衰减：∫ σs·L_amb·exp(−σt·t) dt = σs·L_amb·(1−e^{−σt·len})/σt。
    const float amb_r = m.ambient.x() * kAmbientInterfaceTransmittance;
    const float amb_g = m.ambient.y() * kAmbientInterfaceTransmittance;
    const float amb_b = m.ambient.z() * kAmbientInterfaceTransmittance;
    S->x() += m.sigma_s.x() * amb_r * DecayLength(st_r, 1.0f, len_w);
    S->y() += m.sigma_s.y() * amb_g * DecayLength(st_g, 1.0f, len_w);
    S->z() += m.sigma_s.z() * amb_b * DecayLength(st_b, 1.0f, len_w);
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_MEDIUM_MATH_H_

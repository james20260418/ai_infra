// JPOV 水面波浪 —— GL-free 的 CPU 参考模型（与 water_wave_shader.h 同源）
//
// 职责：把「波浪谱 + 大尺度流向（flow map）」编成一个**可求值的高度场**
//   h(x, z, t) = Σ A_i · sin(k_i·dot(dir_i, p.xz) − ω_i·t + φ_i)
// 以及它的解析梯度（法线 = normalize(−∂h/∂x, 1, −∂h/∂z)）。
//
// 两条定稿口径（2026-11-18 Danis）：
//   ① 谱是**解析函数**，不是纹理：求交要在任意 (x,z) 反复求 h/梯度，
//      只有解析函数能做到无限精度、无采样走样；flow map 才是纹理（大尺度低频）。
//   ② 纯高度场 Gerstner（**无水平位移/choppy**）：求交必须是单值高度图，
//      choppy 会把求交变成解隐式方程，复杂度与坑都上一个台阶，留给后续版本。
//
// ⚠️ 改这里的公式必须同步改 water_wave_shader.h（两处都标了「同源」），
//    由 test/water:water_wave_math_test + water_skin_gpu_test 的逐像素对拍兜底。
//
// ⚠️ flow 方向的空间导数被忽略（dir_eff 在每点被当作常量）：flow map 是低频场，
//    它的 ∂/∂p 项比主项小一到两个数量级；忽略它换来「梯度 = 解析式」的简单性。
//    flow 本身对相位/方向的影响**没有**忽略。

#ifndef JPOV_SRC_WATER_WATER_WAVE_MATH_H_
#define JPOV_SRC_WATER_WATER_WAVE_MATH_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"   // WaterWave / WaterWaves（配置的唯一出处）

namespace jpov {
namespace water {

// 重力加速度（m/s²）。深水色散 ω = sqrt(g·k)。
inline constexpr float kWaveGravity = 9.81f;

// 二分次数：区间半宽 = ΣA/|rd.y|，12 次 ⇒ 精度 = 区间/4096（ΣA=1 m、|rd.y|=0.3 时 ~0.8 mm）。
// 用二分而不是 Newton：区间端点必有变号（|h| ≤ ΣA），全局收敛且相邻像素连续
// （Newton 在波谷/陡坡处 f'→0 会跳根，实测表现为法线噪点）。
inline constexpr int kWaveBisectIterations = 12;

// 高度场采样结果：相对 level_y 的**高度偏移**（米）与梯度（无量纲）。
struct WaterWaveSample {
    float height_m = 0.0f;   // h（米，相对 level_y；正 = 高出基准面）
    float dh_dx = 0.0f;      // ∂h/∂x
    float dh_dz = 0.0f;      // ∂h/∂z
};

// 低频位移场 u(p)（米）与它的空间导数（域扭曲 / domain warp）。
// u = Σ_j A_j·dir_j·sin(k_j·dot(dir_j, p) + φ_j)；相位偏移 θ_i += k_i·dot(u, d_i)。
struct WaveWarpSample {
    float ux = 0.0f;
    float uz = 0.0f;
    float dux_dx = 0.0f;
    float dux_dz = 0.0f;
    float duz_dx = 0.0f;
    float duz_dz = 0.0f;
};

// 采样低频位移场。warp_amplitude_m = 0 ⇒ 全 0（旧行为）。
// Pre-condition: warp_wavelength_m[j] > 0（配置校验在 renderer 侧）。
inline WaveWarpSample SampleWaveWarp(const WaterWaves& waves, float x, float z) {
    WaveWarpSample out;
    if (!(waves.warp_amplitude_m > 0.0f)) {
        return out;
    }
    const float kTwoPi = 6.28318530717958647692f;
    for (int j = 0; j < WaterWaves::kWarpCarriers; ++j) {
        const float lam = waves.warp_wavelength_m[j];
        if (!(lam > 0.0f)) {
            continue;
        }
        float dx = waves.warp_dir_x[j];
        float dz = waves.warp_dir_z[j];
        const float dl = std::sqrt(dx * dx + dz * dz);
        if (dl > 1.0e-6f) {
            dx /= dl;
            dz /= dl;
        } else {
            dx = 1.0f;
            dz = 0.0f;
        }
        const float k = kTwoPi / lam;
        const float ph = k * (dx * x + dz * z) + waves.warp_phase_rad[j];
        const float s = std::sin(ph);
        const float c = std::cos(ph);
        const float a = waves.warp_amplitude_m;
        out.ux += a * dx * s;
        out.uz += a * dz * s;
        out.dux_dx += a * dx * k * dx * c;
        out.dux_dz += a * dx * k * dz * c;
        out.duz_dx += a * dz * k * dx * c;
        out.duz_dz += a * dz * k * dz * c;
    }
    return out;
}

// 域扭曲的「带宽上限」：|∂u/∂p| ≤ U = A·Σ_j k_j（A 为每条载波的位移幅度）。
//
// 一条波长 λ（k = 2π/λ）的波被 warp 之后，沿自身方向的相位梯度是
//     ∂θ/∂s = k·(1 + d·(∂u/∂p)·d)，
// 相对变化 |d·∂u·d| ≤ U。所以：
//   · k·U ≪ 1 ⇒ 只是把波峰推来推去（想要的「掰弯」）；
//   · k·U ≳ 1 ⇒ 波峰被压扁、间距忽长忽短；k·U > 1 处相位反转 ⇒ 折叠成漩涡。
// 实测：A=16 m + 载波 37/64/112/197 m ⇒ U≈4.05，34 m 主波的 λ_eff 在 [−75, +147] m
// 之间扫；1 m 级短波的相位被 k·u 打散成拉丝。
// 结论：warp 必须**按波限带** —— 长波照旧掰弯，短波几乎不动。
// 实现：每条波把 warp 相位/梯度乘 min(1, kWarpSlopeLimit / (k·U))。
inline constexpr float kWarpSlopeLimit = 0.25f;

inline float WaveWarpSlopeBound(const WaterWaves& waves) {
    if (!(waves.warp_amplitude_m > 0.0f)) {
        return 0.0f;
    }
    const float kTwoPi = 6.28318530717958647692f;
    float sum_k = 0.0f;
    for (int j = 0; j < WaterWaves::kWarpCarriers; ++j) {
        const float lam = waves.warp_wavelength_m[j];
        if (lam > 0.0f) {
            sum_k += kTwoPi / lam;
        }
    }
    return waves.warp_amplitude_m * sum_k;
}

// 标准 smoothstep（u 先 clamp 到 [0,1]）。
inline float SmoothStep01(float u) {
    const float c = std::clamp(u, 0.0f, 1.0f);
    return c * c * (3.0f - 2.0f * c);
}

// 远处淡出系数 ∈ [0,1]：1 = 全振幅，0 = 回到纯平面。
// 按**相机水平距离**淡出（不是视轴深度）：远处高频波长小于像素时就该消失，
// 否则 Newton 求交会在远处抖动、法线走样成噪点。
inline float WaveFadeFactor(const WaterWaves& waves, float horizontal_dist_m) {
    if (waves.fade_end_m <= waves.fade_start_m) {
        return 1.0f;   // 未配置淡出 ⇒ 恒 1
    }
    if (horizontal_dist_m <= waves.fade_start_m) {
        return 1.0f;
    }
    if (horizontal_dist_m >= waves.fade_end_m) {
        return 0.0f;
    }
    const float x = (horizontal_dist_m - waves.fade_start_m) /
                    (waves.fade_end_m - waves.fade_start_m);
    return 1.0f - x * x * (3.0f - 2.0f * x);   // smoothstep
}

// ─────────────────────────────────────────────────────────────────────────────
// 法线场过滤：远处亚像素波 → BRDF 粗糙度
//
// 量：MSS = E|∇h|²（斜率均方，无量纲）。各向同性下 GGX 的 a2 = 2σ² = MSS，
//     所以这个数可以直接当 a2 用（与 `normal_var_px` 同量纲）。
//
// 唯一来源：**这份波谱自己**。被像素足迹截断的那部分波不再贡献几何，
// 但它们的方差不能丢，必须转进 BRDF（否则远处退化成一面临界平面镜）。
// 4 个波之外的短波一概不建模 —— 想更粗糙就加波或加振幅，不引入别的参数。
// ─────────────────────────────────────────────────────────────────────────────

// 送进 BRDF 的方差 = “被截断的 4 波方差” + 短波方差（张量相加）。
//   pixel_footprint_m ≤ 0 ⇒ 不做截断（近场，只剩短波）。
//   fade 与 WaveSampleField 同口径（远处淡出的振幅也不再贡献方差）。
// 斜率方差张量（xz 平面）：xx = E[(∂h/∂x)²]，xz = E[∂h/∂x·∂h/∂z]，zz = E[(∂h/∂z)²]。
struct SlopeVariance {
    float xx = 0.0f;
    float xz = 0.0f;
    float zz = 0.0f;
};

// 短波的贡献：总量 slope_rms²，按 2.7:1 分到沿风/横风（Cox–Munk 实测比例）。
inline float CoxMunkMeanSquareSlope(float wind_speed_ms) {
    if (!(wind_speed_ms > 0.0f)) {
        return 0.0f;
    }
    const float u = std::min(wind_speed_ms, 20.0f);
    return 0.00512f * u + 0.003f + 0.00192f * u;
}

inline SlopeVariance ShortWaveVariance(const WaterWaves& waves) {
    SlopeVariance v;
    const float mss = CoxMunkMeanSquareSlope(waves.short_wave.wind_speed_ms);
    if (!(mss > 0.0f)) {
        return v;
    }
    float dx = waves.short_wave.dir_x;
    float dz = waves.short_wave.dir_z;
    const float dl = std::sqrt(dx * dx + dz * dz);
    if (dl > 1.0e-6f) {
        dx /= dl;
        dz /= dl;
    } else {
        return SlopeVariance{mss * 0.5f, 0.0f, mss * 0.5f};   // 无方向 ⇒ 各向同性
    }
    const float along = mss * 0.73f;    // 2.7:1 ⇒ 0.73 / 0.27
    const float cross = mss * 0.27f;
    const float px = -dz;
    const float pz = dx;
    v.xx = along * dx * dx + cross * px * px;
    v.xz = along * dx * dz + cross * px * pz;
    v.zz = along * dz * dz + cross * pz * pz;
    return v;
}

inline SlopeVariance WaveFilteredVariance(const WaterWaves& waves, float pixel_footprint_m,
                                          float fade) {
    SlopeVariance v;
    if (!waves.enabled || fade <= 0.0f) {
        return v;   // 波浪整个关掉 = 镜面水：短波也不参与（与 GLSL 同口径）
    }
    v = ShortWaveVariance(waves);
    const int n = std::min(waves.num_waves, WaterWaves::kMaxWaves);
    const int nd = std::min(waves.num_detail_waves, WaterWaves::kMaxDetailWaves);
    for (int i = 0; i < n + nd; ++i) {
        // 0..n-1：主波；n..n+nd-1：只进法线的细节短波（被足迹截断的部分也必须折进方差）。
        const WaterWave& wv = (i < n) ? waves.waves[i] : waves.detail_waves[i - n];
        const float k = 6.283185307179586f / wv.wavelength_m;
        const float amp = wv.amplitude_m * fade;
        float lod_amp = 1.0f;
        if (pixel_footprint_m > 1.0e-6f) {
            // ⚠️ 必须用 wv（主波或细节波自己）；旧版写死 waves.waves[i]，
            //    i ≥ num_waves 时既用错波长又越过 waves[8] 读 detail_waves 的内存。
            const float wavelength_px = wv.wavelength_m / pixel_footprint_m;
            if (wavelength_px <= 2.0f) {
                lod_amp = 0.0f;
            } else if (wavelength_px < 8.0f) {
                lod_amp = SmoothStep01((wavelength_px - 2.0f) / 6.0f);
            }
        }
        const float a = amp * k;
        const float w = 0.5f * a * a * (1.0f - lod_amp * lod_amp);
        float dx = wv.dir_x;
        float dz = wv.dir_z;
        const float dl = std::sqrt(dx * dx + dz * dz);
        if (dl > 1.0e-6f) {
            dx /= dl;
            dz /= dl;
        } else {
            dx = 1.0f;
            dz = 0.0f;
        }
        v.xx += w * dx * dx;
        v.xz += w * dx * dz;
        v.zz += w * dz * dz;
    }
    return v;
}

// 细节短波对法线的贡献（**只是梯度，不改高度**）。
// 与主波同一份相位/色散/淡出/足迹规则，但不进求交（否则二分成本 ×N）。
// Pre-condition: wavelength_m > 0。
inline void AddDetailWaveGradients(const WaterWaves& waves, float x, float z, float time_s,
                                   float fade, float pixel_footprint_m,
                                   const WaveWarpSample& warp, WaterWaveSample* out) {
    if (out == nullptr || !waves.enabled || fade <= 0.0f) {
        return;
    }
    const int nd = std::clamp(waves.num_detail_waves, 0, WaterWaves::kMaxDetailWaves);
    const float kTwoPi = 6.28318530717958647692f;
    const float warp_u_slope = WaveWarpSlopeBound(waves);
    for (int i = 0; i < nd; ++i) {
        const WaterWave& wv = waves.detail_waves[i];
        float dx = wv.dir_x;
        float dz = wv.dir_z;
        const float dl = std::sqrt(dx * dx + dz * dz);
        if (dl > 1.0e-6f) {
            dx /= dl;
            dz /= dl;
        } else {
            dx = 1.0f;
            dz = 0.0f;
        }
        const float k = kTwoPi / wv.wavelength_m;
        const float omega = std::sqrt(kWaveGravity * k) * waves.speed_scale;
        float lod_amp = 1.0f;
        if (pixel_footprint_m > 1.0e-6f) {
            const float wavelength_px = wv.wavelength_m / pixel_footprint_m;
            if (wavelength_px <= 2.0f) {
                lod_amp = 0.0f;
            } else if (wavelength_px < 8.0f) {
                lod_amp = SmoothStep01((wavelength_px - 2.0f) / 6.0f);
            }
        }
        const float amp = wv.amplitude_m * fade * lod_amp;
        const float warp_scale =
            (warp_u_slope > 0.0f) ? std::min(1.0f, kWarpSlopeLimit / (k * warp_u_slope))
                                  : 1.0f;
        const float wk = k * warp_scale;
        const float wx = wk * (warp.dux_dx * dx + warp.duz_dx * dz);
        const float wz = wk * (warp.dux_dz * dx + warp.duz_dz * dz);
        const float phase = k * (dx * x + dz * z) - omega * time_s + wv.phase_rad +
                            wk * (warp.ux * dx + warp.uz * dz);
        const float cos_p = std::cos(phase);
        out->dh_dx += amp * (k * dx + wx) * cos_p;
        out->dh_dz += amp * (k * dz + wz) * cos_p;
    }
}

// 采样高度场（含 flow 调制）。
//   flow_x / flow_z  该点的 flow 向量（世界水平方向 × 强度；0 = 无流）。
//                    GPU 侧由 flow map 纹理采样得到，CPU 侧由调用方给出。
//   fade             WaveFadeFactor 的结果（0 ⇒ 直接返回 0，省掉三角函数的调用）。
//   pixel_footprint_m 该像素在水面上覆盖的世界尺寸（米）；0 = 不做抗锯齿衰减。
//                    一个波长只占 <2 px 时该波必须消失（否则法线逐像素乱跳 ⇒
//                    远处反射/碎光变成麻点）。这是「按像素足迹的谱截断」，不是 mip。
// Pre-condition: num_waves ∈ [0, WaterWaves::kMaxWaves]；wavelength_m > 0。
inline WaterWaveSample SampleWaveField(const WaterWaves& waves, float x, float z,
                                       float time_s, float flow_x, float flow_z,
                                       float fade, float pixel_footprint_m = 0.0f) {
    WaterWaveSample out;
    if (!waves.enabled || waves.num_waves <= 0 || fade <= 0.0f) {
        return out;
    }

    const float flow_len = std::sqrt(flow_x * flow_x + flow_z * flow_z);
    const bool has_flow = flow_len > 1.0e-6f;
    float flow_dx = 0.0f;
    float flow_dz = 0.0f;
    if (has_flow) {
        flow_dx = flow_x / flow_len;
        flow_dz = flow_z / flow_len;
    }
    // 方向混合量：流越强，波越朝流方向（[0,1]）。
    const float blend = has_flow
                            ? std::clamp(waves.flow_align * flow_len, 0.0f, 1.0f)
                            : 0.0f;
    // 风浪区振幅增益：流越强浪越大。
    const float amp_gain = 1.0f + waves.flow_wind_gain * std::clamp(flow_len, 0.0f, 1.0f);

    const float kTwoPi = 6.28318530717958647692f;
    const WaveWarpSample warp = SampleWaveWarp(waves, x, z);
    // warp 限带（见 WaveWarpSlopeBound）：k·U 大的短波几乎不吃位移。
    const float warp_u_slope = WaveWarpSlopeBound(waves);
    const int count = std::clamp(waves.num_waves, 0, WaterWaves::kMaxWaves);
    for (int i = 0; i < count; ++i) {
        const WaterWave& w = waves.waves[i];
        float dx = w.dir_x;
        float dz = w.dir_z;
        const float dir_len = std::sqrt(dx * dx + dz * dz);
        if (dir_len > 1.0e-6f) {
            dx /= dir_len;
            dz /= dir_len;
        }
        if (has_flow) {
            const float mx = (1.0f - blend) * dx + blend * flow_dx;
            const float mz = (1.0f - blend) * dz + blend * flow_dz;
            const float ml = std::sqrt(mx * mx + mz * mz);
            if (ml > 1.0e-4f) {
                dx = mx / ml;
                dz = mz / ml;
            }
        }

        const float k = kTwoPi / w.wavelength_m;
        const float omega = std::sqrt(kWaveGravity * k) * waves.speed_scale;
        // 抗锯齿：λ 占的像素数 ≤ 2 ⇒ 消失；≥ 8 ⇒ 全幅（中间 smoothstep）。
        float lod_amp = 1.0f;
        if (pixel_footprint_m > 1.0e-6f) {
            const float wavelength_px = w.wavelength_m / pixel_footprint_m;
            if (wavelength_px <= 2.0f) {
                lod_amp = 0.0f;
            } else if (wavelength_px < 8.0f) {
                lod_amp = SmoothStep01((wavelength_px - 2.0f) / 6.0f);
            }
        }
        const float amp = w.amplitude_m * amp_gain * fade * lod_amp;
        // 域扭曲：θ += k·dot(u, d)，梯度多一项 k·(∂u/∂x, ∂u/∂z)·d。
        // 限带：乘 warp_scale（k·U 大的短波几乎不动，见 WaveWarpSlopeBound）。
        const float warp_scale =
            (warp_u_slope > 0.0f) ? std::min(1.0f, kWarpSlopeLimit / (k * warp_u_slope))
                                  : 1.0f;
        const float wk = k * warp_scale;
        const float wx = wk * (warp.dux_dx * dx + warp.duz_dx * dz);
        const float wz = wk * (warp.dux_dz * dx + warp.duz_dz * dz);
        const float phase = k * (dx * x + dz * z) - omega * time_s + w.phase_rad +
                            wk * (warp.ux * dx + warp.uz * dz);
        const float sin_p = std::sin(phase);
        const float cos_p = std::cos(phase);
        const float gx = k * dx + wx;
        const float gz = k * dz + wz;

        out.height_m += amp * sin_p;
        out.dh_dx += amp * gx * cos_p;
        out.dh_dz += amp * gz * cos_p;
        if (waves.stokes_second_order) {
            // −½·k·A²·cos 2θ：峰抬尖、谷抬平。cos2θ = 1 − 2sin²θ，sin2θ = 2sinθcosθ。
            const float sin2 = 2.0f * sin_p * cos_p;
            out.height_m += -0.5f * k * amp * amp * (1.0f - 2.0f * sin_p * sin_p);
            out.dh_dx += k * amp * amp * sin2 * gx;
            out.dh_dz += k * amp * amp * sin2 * gz;
        }
    }
    return out;
}

// 高度上界：Σ A_i · amp_gain · fade（含 flow 的风浪增益）。
// 用于二分求交的区间半径 —— |h| 永远不超过它（fade∈[0,1]）。
// 有效振幅上界 ΣA·fade·lod_amp —— 必须与 GLSL 的 WaveAmplitudeSum(fade, footprint) 同口径，
// 否则二分区间在 GPU/CPU 之间不一致（成片差异只在掠射区露出来）。
inline float WaveAmplitudeSum(const WaterWaves& waves, float flow_x, float flow_z,
                              float fade, float pixel_footprint_m = 0.0f) {
    if (!waves.enabled || waves.num_waves <= 0 || fade <= 0.0f) {
        return 0.0f;
    }
    const float flow_len = std::sqrt(flow_x * flow_x + flow_z * flow_z);
    const float amp_gain = 1.0f + waves.flow_wind_gain * std::clamp(flow_len, 0.0f, 1.0f);
    const int count = std::clamp(waves.num_waves, 0, WaterWaves::kMaxWaves);
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        // 与 SampleWaveField 的 anti-aliasing 截断同一份权重
        float lod_amp = 1.0f;
        if (pixel_footprint_m > 1.0e-6f) {
            const float wavelength_px = waves.waves[i].wavelength_m / pixel_footprint_m;
            if (wavelength_px <= 2.0f) {
                lod_amp = 0.0f;
            } else if (wavelength_px < 8.0f) {
                lod_amp = SmoothStep01((wavelength_px - 2.0f) / 6.0f);
            }
        }
        const float a = waves.waves[i].amplitude_m * lod_amp;
        sum += a;
        if (waves.stokes_second_order) {
            // 二阶谐波把峰值再抬高 ½·k·A²：二分区间必须覆盖它，否则端点不变号。
            const float k = 6.28318530717958647692f / waves.waves[i].wavelength_m;
            sum += 0.5f * k * a * a;
        }
    }
    return sum * amp_gain * fade;
}

// 由梯度求法线（单位向量，y > 0）。h ≡ 0 ⇒ (0,1,0)。
inline Vec3f WaveNormal(const WaterWaveSample& sample) {
    const float nx = -sample.dh_dx;
    const float ny = 1.0f;
    const float nz = -sample.dh_dz;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1.0e-8f) {
        return Vec3f{0.0f, 1.0f, 0.0f};
    }
    return Vec3f{nx / len, ny / len, nz / len};
}

// 视线与波浪曲面的交点。
struct WaveSurfaceHit {
    bool hit = false;
    float t = 0.0f;                          // 射线参数（米，rd 为单位向量）
    Vec3f normal = {0.0f, 1.0f, 0.0f};       // 交点法线（单位，y > 0）
    WaterWaveSample sample;                  // 交点的 h / 梯度
};

// 求交 = 平面初值 + Newton 修正：
//   f(t)  = (cam.y + rd.y·t − level) − h(cam.xz + rd.xz·t)
//   f'(t) = rd.y − (∂h/∂x·rd.x + ∂h/∂z·rd.z)
// 无波浪时**不走迭代**，直接返回平面解（与旧版逐字节一致）。
//
// ⚠️ flow 在整条射线上取**同一个值**（调用方传入）：CPU 参考用于单测/对拍，
//    GPU 侧每次迭代按当前 P 采样纹理。两者在常量 flow 下等价。
//
// Pre-condition: rd 为单位向量。
inline bool IntersectWaveSurface(const Vec3f& cam_pos, const Vec3f& rd, float level_y,
                                 const WaterWaves& waves, float flow_x, float flow_z,
                                 float time_s, float pixel_footprint_m,
                                 WaveSurfaceHit* out /*output*/) {
    if (out == nullptr) {
        return false;
    }
    *out = WaveSurfaceHit{};

    if (std::fabs(rd.y()) < 1.0e-6f) {
        return false;   // 与水平面平行：没有唯一交点
    }
    float t = (level_y - cam_pos.y()) / rd.y();
    if (t <= 0.0f) {
        return false;   // 交点在相机后方
    }

    if (!waves.enabled || waves.num_waves <= 0) {
        out->hit = true;
        out->t = t;
        out->normal = Vec3f{0.0f, 1.0f, 0.0f};
        return true;
    }

    // `f(t) = (cam.y + rd.y·t − level) − h(p.xz)`
    // 在 t ∈ [t_plane − span, t_plane + span] 两端**必然异号**（|h| ≤ ΣA = span·|rd.y|），
    // 所以二分是全局收敛的；相邻像素的区间连续 ⇒ 收敛点也连续 ⇒ 不会出现「交点乱跳」。
    // （Newton 在波谷/陡坡处 f' = rd.y − ∇h·rd.xz 会趋近 0，dt 爆炸或跳到相邻根 ——
    //   实测就是「浪坑里一批法线像噪点」的来源。）
    // 区间半径用**平面交点处的有效振幅**（含 fade；足迹截断在 CPU 参考里默认关）。
    // 与 GLSL 同口径：远处谱已截断成平面 ⇒ span→0 ⇒ t 连续。
    const float t_plane = t;
    const Vec3f p_plane{cam_pos.x() + rd.x() * t_plane, cam_pos.y() + rd.y() * t_plane,
                        cam_pos.z() + rd.z() * t_plane};
    const float dxp = p_plane.x() - cam_pos.x();
    const float dzp = p_plane.z() - cam_pos.z();
    const float fade_bound = WaveFadeFactor(waves, std::sqrt(dxp * dxp + dzp * dzp));
    const float amp_sum = WaveAmplitudeSum(waves, flow_x, flow_z, fade_bound, pixel_footprint_m);

    const auto height_at = [&](float tt) {
        const Vec3f p{cam_pos.x() + rd.x() * tt,
                      cam_pos.y() + rd.y() * tt,
                      cam_pos.z() + rd.z() * tt};
        const float dx = p.x() - cam_pos.x();
        const float dz = p.z() - cam_pos.z();
        const float dist = std::sqrt(dx * dx + dz * dz);
        return SampleWaveField(waves, p.x(), p.z(), time_s, flow_x, flow_z,
                               WaveFadeFactor(waves, dist), pixel_footprint_m)
            .height_m;
    };

    // 法线 only（debug）：求交仍用平面解，只把法线换成该点的梯度。
    if (!waves.displacement) {
        const Vec3f p{cam_pos.x() + rd.x() * t,
                      cam_pos.y() + rd.y() * t,
                      cam_pos.z() + rd.z() * t};
        const float dx = p.x() - cam_pos.x();
        const float dz = p.z() - cam_pos.z();
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float fade_here = WaveFadeFactor(waves, dist);
        WaterWaveSample s =
            SampleWaveField(waves, p.x(), p.z(), time_s, flow_x, flow_z, fade_here,
                            pixel_footprint_m);
        AddDetailWaveGradients(waves, p.x(), p.z(), time_s, fade_here, pixel_footprint_m,
                               SampleWaveWarp(waves, p.x(), p.z()), &s);
        out->hit = true;
        out->t = t;
        out->sample = s;
        out->normal = WaveNormal(s);
        return true;
    }

    const float rdy = (std::fabs(rd.y()) < 1.0e-3f) ? 1.0e-3f : std::fabs(rd.y());
    const float span = amp_sum / rdy;
    float t_lo = t_plane - span;
    float t_hi = t_plane + span;
    if (t_lo < 1.0e-4f) {
        t_lo = 1.0e-4f;
    }
    if (t_hi <= t_lo) {
        t_hi = t_lo + 1.0e-4f;
    }

    const auto f_of_t = [&](float tt) {
        const float p_y = cam_pos.y() + rd.y() * tt;
        return (p_y - level_y) - height_at(tt);
    };

    const float f_lo = f_of_t(t_lo);
    const float f_hi = f_of_t(t_hi);
    if ((f_lo > 0.0f) == (f_hi > 0.0f)) {
        // 区间没有变号（理论上不会发生；极端 fade/参数下兜底回平面解）。
        out->hit = true;
        out->t = t_plane;
        out->normal = Vec3f{0.0f, 1.0f, 0.0f};
        return true;
    }
    bool lo_positive = (f_lo > 0.0f);
    for (int i = 0; i < kWaveBisectIterations; ++i) {
        const float tm = 0.5f * (t_lo + t_hi);
        const bool m_positive = (f_of_t(tm) > 0.0f);
        if (m_positive == lo_positive) {
            t_lo = tm;
        } else {
            t_hi = tm;
        }
    }
    t = 0.5f * (t_lo + t_hi);
    if (t <= 0.0f) {
        return false;
    }

    const Vec3f p{cam_pos.x() + rd.x() * t,
                  cam_pos.y() + rd.y() * t,
                  cam_pos.z() + rd.z() * t};
    const float dx = p.x() - cam_pos.x();
    const float dz = p.z() - cam_pos.z();
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float fade_here = WaveFadeFactor(waves, dist);
    WaterWaveSample s =
        SampleWaveField(waves, p.x(), p.z(), time_s, flow_x, flow_z, fade_here,
                        pixel_footprint_m);
    AddDetailWaveGradients(waves, p.x(), p.z(), time_s, fade_here, pixel_footprint_m,
                           SampleWaveWarp(waves, p.x(), p.z()), &s);
    out->hit = true;
    out->t = t;
    out->sample = s;
    out->normal = WaveNormal(s);
    return true;
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_MATH_H_

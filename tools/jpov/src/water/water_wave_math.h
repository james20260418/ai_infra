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
        const float phase = k * (dx * x + dz * z) - omega * time_s + w.phase_rad;
        const float sin_p = std::sin(phase);
        const float cos_p = std::cos(phase);

        out.height_m += amp * sin_p;
        out.dh_dx += amp * k * dx * cos_p;
        out.dh_dz += amp * k * dz * cos_p;
    }
    return out;
}

// 高度上界：Σ A_i · amp_gain · fade（含 flow 的风浪增益）。
// 用于二分求交的区间半径 —— |h| 永远不超过它（fade∈[0,1]）。
inline float WaveAmplitudeSum(const WaterWaves& waves, float flow_x, float flow_z,
                              float fade) {
    if (!waves.enabled || waves.num_waves <= 0 || fade <= 0.0f) {
        return 0.0f;
    }
    const float flow_len = std::sqrt(flow_x * flow_x + flow_z * flow_z);
    const float amp_gain = 1.0f + waves.flow_wind_gain * std::clamp(flow_len, 0.0f, 1.0f);
    const int count = std::clamp(waves.num_waves, 0, WaterWaves::kMaxWaves);
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        sum += waves.waves[i].amplitude_m;
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
    const float fade_bound = 1.0f;   // 区间半径取上界（不含 fade 衰减，保守）
    const float amp_sum = WaveAmplitudeSum(waves, flow_x, flow_z, fade_bound);
    const float t_plane = t;

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
        const WaterWaveSample s =
            SampleWaveField(waves, p.x(), p.z(), time_s, flow_x, flow_z,
                            WaveFadeFactor(waves, dist), pixel_footprint_m);
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
    const WaterWaveSample s =
        SampleWaveField(waves, p.x(), p.z(), time_s, flow_x, flow_z,
                        WaveFadeFactor(waves, dist), pixel_footprint_m);
    out->hit = true;
    out->t = t;
    out->sample = s;
    out->normal = WaveNormal(s);
    return true;
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_MATH_H_

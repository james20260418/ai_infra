// 海面「人类可读」配置（uniform 版）—— 艺术层，不含任何渲染实现细节。
//
// 分层（2026-11-20 Danis 定）：
//   WaterConfig（这一份，用户只碰它）
//        │  BuildWater()（本文件，纯 CPU / 可单测 / 只在配置变化时算一次）
//        ▼
//   WaterWaves（谱：8 主波 + 64 细节 + 短波 MSS）+ WaterReadout（只读摘要）
//        │  每帧只上传 uniform + 推进 WaterCommand::time_s
//        ▼
//   GLSL（src/water/water_wave_shader.h）
//
// 三条语义约定（都是踩过的坑，别改）：
//  ① 风/涌的方向 = **吹向 / 传向**，不是气象的"来向"。给"来向"的 UI 在这里转一次。
//  ② 风区 = ∞ 表示充分成长，**不要用 0**（0 物理上是"没有风区"，和"无限"共用魔数最易错）。
//  ③ 涌是 (dir_x, dir_z, 周期 s, 波高 Hs) 四个数（等价于 Vec4，但用具名字段更好读）：
//     方向是二维的，拆成 dir_deg 会让以后做 map 时方向插值在 ±180° 翻转。
//
// 随机性：`seed` 每份配置一个（不是全局单例），生成用 **按索引哈希** 而不是流式 RNG ——
// 多一条波不会把后面所有波的相位挪位，也不受并行顺序影响。

#ifndef JPOV_INTERFACE_WATER_CONFIG_H_
#define JPOV_INTERFACE_WATER_CONFIG_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "tools/jpov/interface/render_command.h"

namespace jpov {
namespace water {

// 深水判据用的厚度（米）。与 src/water/water_wave_math.h 的 kDeepWaterDepthM 同值
// （那边是渲染侧默认值；这里是配置侧，故意不互相 include，避免 interface→src 依赖）。
inline constexpr float kOceanDeepDepthM = 300.0f;
inline constexpr float kOceanGravity = 9.81f;
// tanh 饱和门槛：k·d > 20 ⇒ 深水（tanh 与 1 的差 < 1e-17）。
inline constexpr float kOceanDeepKdThreshold = 20.0f;

// 深水色散（配置侧用；渲染侧同公式见 water_wave_math.h::DispersionOmega）。
inline float OceanOmega(float k, float depth_m) {
    if (!(k > 0.0f)) {
        return 0.0f;
    }
    if (!(depth_m > 0.0f)) {
        return std::sqrt(kOceanGravity * k);
    }
    const float kd = k * depth_m;
    const float th = (kd > kOceanDeepKdThreshold) ? 1.0f : std::tanh(kd);
    return std::sqrt(kOceanGravity * k * th);
}

// 一份"水"：地域无差异（uniform），可序列化、可缓存、可直接给美术调。
struct WaterConfig {
    // ── ① 风（局部风海）────────────────────────────────────────────
    // 世界 xz 平面的风速度向量：|v| = 10 m 高处风速（m/s），方向 = **吹向**。
    // 面板上是"风速 + 风向"两条滑条，写回时用 SetWind(speed, dir_deg)。
    Vec3f wind_velocity = {0.0f, 0.0f, 0.0f};   // y 恒 0
    // 风区（km）与风时（h）。∞ = 不限制（充分成长）。两者取先饱和的那个。
    float fetch_km = std::numeric_limits<float>::infinity();
    float duration_h = std::numeric_limits<float>::infinity();

    // ── ② 涌（远方传来的长波系统）────────────────────────────────
    // 涌与风海是**两套**系统，方向可以不同 —— 真实海最好看的部分就在这里。
    // 字段顺序即语义：(dir_x, dir_z) 传向、周期 s、波高 Hs m；波高 = 0 ⇒ 无涌。
    struct Swell {
        float dir_x = 1.0f;
        float dir_z = 0.0f;
        float period_s = 9.5f;
        float height_m = 0.0f;
    };
    Swell swell;

    // ── ③ 介质 ──────────────────────────────────────────────────
    Vec3f sigma_a = {0.50f, 0.054f, 0.014f};   // 1/m（逐通道吸收 = 水色）
    float sigma_s = 0.04f;                     // 1/m（灰，散射 = 浑浊）
    float phase_g = 0.35f;                     // HG 各向异性
    float ior = 1.333f;                        // 折射率
    // 平底深度（米）。既影响色散（浅水变慢变短），也决定海底底板画在哪 ——
    // 一个数，因为它俩物理上就是同一件事。默认 3.5 m = "看得见底的浅海"。
    float depth_m = 3.5f;
    float level_y = 0.0f;                      // 水位

    // ── ④ 相位随机种子（每份配置一个；0 = 出厂默认实现）──────────
    uint32_t seed = 0;

    // 便捷读写（面板/脚本用；不是额外状态）。
    float WindSpeedMs() const {
        return std::sqrt(wind_velocity.x() * wind_velocity.x() +
                         wind_velocity.z() * wind_velocity.z());
    }
    // 吹向的方位角（度，[0,360)；xz 平面，+z 为 0°，+x 为 90°）。
    float WindDirDeg() const {
        const float a = std::atan2(wind_velocity.x(), wind_velocity.z());
        return a < 0.0f ? a + 6.28318530717958647692f : a;
    }
    void SetWind(float speed_ms, float dir_rad) {
        wind_velocity = {speed_ms * std::sin(dir_rad), 0.0f, speed_ms * std::cos(dir_rad)};
    }
    bool HasSwell() const { return swell.height_m > 0.0f && swell.period_s > 0.0f; }
    float SwellDirDeg() const {
        const float a = std::atan2(swell.dir_x, swell.dir_z);
        return a < 0.0f ? a + 6.28318530717958647692f : a;
    }
    void SetSwell(float hs_m, float period_s, float dir_rad) {
        swell.dir_x = std::sin(dir_rad);
        swell.dir_z = std::cos(dir_rad);
        swell.period_s = period_s;
        swell.height_m = hs_m;
    }
};

// 设备/性能档位：**不进资产**（同一份 WaterConfig 在不同机器上应出同一张图）。
struct WaterQuality {
    int detail_waves = 64;      // 32 / 64 / 128
    int main_waves = 8;         // 4 / 8（有涌时会被涌占用 3 条）
};

// 只读摘要（面板显示 / 断言用）。
struct WaterReadout {
    float hs_wind_m = 0.0f;      // 风海有效波高
    float hs_swell_m = 0.0f;     // 涌有效波高
    float hs_total_m = 0.0f;     // sqrt(风海² + 涌²)
    float period_s = 0.0f;       // 峰周期（风海；有涌时取两者中能量大的）
    float lambda_peak_m = 0.0f;  // 峰波长
    float rms_slope = 0.0f;      // 斜坡均方根（陡度，视觉强度的量度）
};

// ── 确定性哈希 RNG（按索引取，不是顺序流）────────────────────────────
inline uint32_t OceanHash(uint32_t seed, uint32_t i) {
    uint64_t x = (static_cast<uint64_t>(seed) << 32) ^ (i + 0x9E3779B97F4A7C15ull);
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return static_cast<uint32_t>(x >> 32);
}
// [0,1) 均匀
inline float OceanRand01(uint32_t seed, uint32_t i) {
    return static_cast<float>(OceanHash(seed, i) & 0xFFFFFFu) / 16777216.0f;
}
// 标准正态（Box–Muller，取一对里的第一个；够用且无状态）
inline float OceanRandNormal(uint32_t seed, uint32_t i) {
    const float u1 = std::max(OceanRand01(seed, 2u * i), 1.0e-7f);
    const float u2 = OceanRand01(seed, 2u * i + 1u);
    return std::sqrt(-2.0f * std::log(u1)) *
           std::cos(6.28318530717958647692f * u2);
}

// Cox–Munk 斜率方差（毛细波/短重力波段）：mss = 0.00512U + 0.003 + 0.00192U。
// ⚠️ 与 src/water/water_wave_math.h::CoxMunkMeanSquareSlope 同源（测试锁一致性）。
// 这里只用它来**按风速缩放细节短波**：细节波不是"固定的粗糙度"，是风持续注入的。
inline float OceanCoxMunkMss(float wind_ms) {
    if (!(wind_ms > 0.0f)) {
        return 0.0f;
    }
    const float u = std::min(wind_ms, 20.0f);
    return 0.00512f * u + 0.003f + 0.00192f * u;
}

// ── 风 → (Hs, Tp)：SMB 经验式（U 取 10 m 高处风速）────────────────────
// 充分成长：Hs∞ = 0.283·U²/g，Tp∞ = 7.54·U/g。
// 风区受限：Hs = Hs∞·tanh[0.0125·(gF/U²)^0.42]，Tp = Tp∞·tanh[0.077·(gF/U²)^0.25]。
// 风时受限：Hs = Hs∞·tanh[0.00565·(gt/U)^0.4]（周期先饱和，只看波高）。
inline void WindSeaHsTp(float u_ms, float fetch_km, float duration_h, float* hs, float* tp) {
    const float g = kOceanGravity;
    const float hs_inf = 0.283f * u_ms * u_ms / g;
    const float tp_inf = 7.54f * u_ms / g;
    *hs = hs_inf;
    *tp = tp_inf;
    if (!(u_ms > 0.01f)) {
        *hs = 0.0f;
        *tp = 0.0f;
        return;
    }
    if (std::isfinite(fetch_km) && fetch_km > 0.0f) {
        const float x = g * (fetch_km * 1000.0f) / (u_ms * u_ms);
        const float hs_f = hs_inf * std::tanh(0.0125f * std::pow(x, 0.42f));
        const float tp_f = tp_inf * std::tanh(0.077f * std::pow(x, 0.25f));
        *hs = std::min(*hs, hs_f);
        *tp = std::min(*tp, tp_f);
    }
    if (std::isfinite(duration_h) && duration_h > 0.0f) {
        const float x = g * (duration_h * 3600.0f) / u_ms;
        const float hs_t = hs_inf * std::tanh(0.00565f * std::pow(x, 0.4f));
        *hs = std::min(*hs, hs_t);
    }
}

// ── 展开：WaterConfig → WaterWaves + 读数 ────────────────────────────
// 纯函数（同输入 ⇒ 同输出；只看 seed，不看时间）。
inline void BuildWater(const WaterConfig& cfg, const WaterQuality& quality,
                       WaterWaves* out_waves, WaterReadout* out_readout) {
    CHECK(out_waves != nullptr);
    WaterWaves w;
    w.enabled = true;
    w.displacement = true;
    w.stokes_second_order = true;
    w.fade_start_m = 300.0f;
    w.fade_end_m = 2000.0f;
    // 默认不启用 flow map（v1 uniform：均匀流在视觉上不可见，见 docs §8.4）。
    w.flow_align = 0.0f;
    w.flow_wind_gain = 0.0f;
    w.flow_strength = 0.0f;

    const float u = cfg.WindSpeedMs();
    float hs_w = 0.0f, tp_w = 0.0f;
    WindSeaHsTp(u, cfg.fetch_km, cfg.duration_h, &hs_w, &tp_w);
    const float two_pi = 6.28318530717958647692f;
    const float lam_p = (tp_w > 0.0f) ? 1.56f * tp_w * tp_w : 0.0f;

    // 主波预算：有涌时给涌留 3 条。
    const int main_cap = std::min(quality.main_waves, WaterWaves::kMaxWaves);
    const int swell_n = cfg.HasSwell() ? std::min(3, main_cap - 1) : 0;
    const int wind_n = std::max(1, main_cap - swell_n);

    const float wind_dir_rad = cfg.WindDirDeg();
    // λ 倍数表（相对峰位）：长波定形，短波靠近细节档。
    static constexpr float kLamMul[8] = {1.60f, 1.25f, 1.00f, 0.80f,
                                         0.63f, 0.50f, 0.40f, 0.32f};
    // 方向散布：风区越大越整齐（20°~32°）。
    const float fetch_km = cfg.fetch_km;
    const float spread_deg = (std::isfinite(fetch_km) && fetch_km >= 300.0f) ? 20.0f : 30.0f;
    const float spread_rad = spread_deg * 0.017453292519943295f;

    int n = 0;
    if (wind_n > 0 && hs_w > 0.0f && lam_p > 0.0f) {
        // A ∝ λ^1.2（长波能量大），再整体归一化到 Hs。
        float sum_a2 = 0.0f;
        float amp[8] = {0.0f};
        for (int i = 0; i < wind_n; ++i) {
            const float lam = lam_p * kLamMul[i % 8];
            amp[i] = std::pow(lam, 1.2f);
            sum_a2 += 0.5f * amp[i] * amp[i];
        }
        const float scale = std::sqrt((hs_w * hs_w / 16.0f) / std::max(sum_a2, 1e-12f));
        for (int i = 0; i < wind_n; ++i) {
            const float lam = lam_p * kLamMul[i % 8];
            const float ang = wind_dir_rad + OceanRandNormal(cfg.seed, 100u + i) * spread_rad;
            WaterWave wv;
            wv.dir_x = std::sin(ang);
            wv.dir_z = std::cos(ang);
            wv.wavelength_m = lam;
            wv.amplitude_m = amp[i] * scale;
            wv.phase_rad = 6.28318530717958647692f * OceanRand01(cfg.seed, 200u + i);
            w.waves[n++] = wv;
        }
    }
    if (swell_n > 0) {
        const float hs_s = cfg.swell.height_m;
        const float tp_s = cfg.swell.period_s;
        const float lam_s = 1.56f * tp_s * tp_s;
        const float dir_s = cfg.SwellDirDeg();
        static constexpr float kSwellMul[3] = {1.15f, 1.00f, 0.87f};
        float sum_a2 = 0.0f;
        for (int i = 0; i < swell_n; ++i) {
            sum_a2 += 0.5f * 1.0f;
        }
        const float scale = std::sqrt((hs_s * hs_s / 16.0f) / std::max(sum_a2, 1e-12f));
        for (int i = 0; i < swell_n; ++i) {
            const float ang = dir_s + OceanRandNormal(cfg.seed, 300u + i) * 0.14f;  // ±8°
            WaterWave wv;
            wv.dir_x = std::sin(ang);
            wv.dir_z = std::cos(ang);
            wv.wavelength_m = lam_s * kSwellMul[i];
            wv.amplitude_m = scale;
            wv.phase_rad = 6.28318530717958647692f * OceanRand01(cfg.seed, 400u + i);
            w.waves[n++] = wv;
        }
    }
    w.num_waves = n;

    // 细节短波：λ 0.3–8 m，等陡度 kA=0.07，方向均匀（各向同性）。
    // 只进法线、不进求交 ⇒ 便宜的"碎光/粗糙度"。
    const int nd = std::clamp(quality.detail_waves, 0, WaterWaves::kMaxDetailWaves);
    // 细节短波的陡度 = 0.07 × sqrt(mss(U)/mss(8))：无风 ⇒ 没有碎波（镜面），
    // 大风 ⇒ 更糙。把它写成固定常数会让"平静"预设也长满碎浪（实测 rms 坡度不变）。
    const float kRefMss = OceanCoxMunkMss(8.0f);
    const float steep = 0.07f * std::sqrt(OceanCoxMunkMss(u) / kRefMss);
    for (int i = 0; i < nd; ++i) {
        const float t0 = OceanRand01(cfg.seed, 1000u + i);
        const float t1 = OceanRand01(cfg.seed, 2000u + i);
        const float lam = std::exp(std::log(0.3f) + t0 * (std::log(8.0f) - std::log(0.3f)));
        WaterWave wv;
        const float ang = 6.28318530717958647692f * t1;
        wv.dir_x = std::sin(ang);
        wv.dir_z = std::cos(ang);
        wv.wavelength_m = lam;
        wv.amplitude_m = steep * lam / two_pi;
        wv.phase_rad = 6.28318530717958647692f * OceanRand01(cfg.seed, 3000u + i);
        w.detail_waves[i] = wv;
    }
    w.num_detail_waves = nd;

    // 短波（亚像素）：方向 = 风向；量由 Cox–Munk 给。
    const float wl = u;
    if (wl > 1.0e-3f) {
        w.short_wave.dir_x = std::sin(wind_dir_rad);
        w.short_wave.dir_z = std::cos(wind_dir_rad);
        w.short_wave.wind_speed_ms = wl;
    } else {
        w.short_wave.dir_x = 0.0f;
        w.short_wave.dir_z = 0.0f;
        w.short_wave.wind_speed_ms = 0.0f;
    }

    *out_waves = w;

    if (out_readout != nullptr) {
        WaterReadout r;
        r.hs_wind_m = hs_w;
        r.hs_swell_m = cfg.HasSwell() ? cfg.swell.height_m : 0.0f;
        r.hs_total_m = std::sqrt(hs_w * hs_w + r.hs_swell_m * r.hs_swell_m);
        r.period_s = (r.hs_swell_m > hs_w) ? cfg.swell.period_s : tp_w;
        r.lambda_peak_m = lam_p;
        // RMS 坡度 = sqrt(Σ ½(kA)²)，只统计"真波"（细节也计入，因为人眼看到的是它们）。
        float mss = 0.0f;
        for (int i = 0; i < w.num_waves; ++i) {
            const float k = two_pi / w.waves[i].wavelength_m;
            mss += 0.5f * (k * w.waves[i].amplitude_m) * (k * w.waves[i].amplitude_m);
        }
        for (int i = 0; i < w.num_detail_waves; ++i) {
            const float k = two_pi / w.detail_waves[i].wavelength_m;
            mss += 0.5f * (k * w.detail_waves[i].amplitude_m) *
                   (k * w.detail_waves[i].amplitude_m);
        }
        r.rms_slope = std::sqrt(mss);
        *out_readout = r;
    }
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_INTERFACE_WATER_CONFIG_H_

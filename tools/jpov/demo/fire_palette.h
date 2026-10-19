// fire_palette.h — 粒子火的「温度 → 颜色」映射（3 套，供氛围对比）
//
// 背景：
//   粒子火（ParticleInstance）本身**不是按温度存的**——EmitterConfig 存的是
//   `color_over_life` 这条 RGBA 色带；温度只是体积火（raymarch / JPVOL）里的通道。
//   这里为了能像体积火那样调「整体温度」，在 viewer 侧引入一个归一化温度
//   `tau ∈ [0,1]`（1=最热），再经下面的 palette 映射成颜色。
//
// 约定：
//   * tau = 1 最热（白热），tau = 0 最冷（暗红/消失）。
//   * 返回颜色可以是 HDR（RGB > 1），最终亮度由调用方乘 alpha / 强度。
//   * 三套映射：
//       0 Blackbody  —— 物理黑体近似（Tanner Helland），1200K → 6500K
//       1 Cinematic  —— 电影暖调：白热→黄→橙→深红
//       2 Cute       —— 可爱风：粉紫白→蜜桃→暖橙→玫瑰暗红
#ifndef JPOV_DEMO_FIRE_PALETTE_H_
#define JPOV_DEMO_FIRE_PALETTE_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_particle {

inline constexpr int kFirePaletteCount = 3;
inline constexpr const char* kFirePaletteNames[kFirePaletteCount] = {
    "Blackbody", "Cinematic", "Cute",
};

// 简单分段线性色带（keys 必须按 t 升序）。
struct PaletteStop { float t; float r, g, b; };

inline jpov::Color PaletteLerp(const PaletteStop* stops, int n, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (n <= 0) { return jpov::Color{1, 1, 1, 1}; }
    if (t <= stops[0].t) { return {stops[0].r, stops[0].g, stops[0].b, 1}; }
    for (int i = 1; i < n; ++i) {
        if (t <= stops[i].t) {
            const float span = std::max(stops[i].t - stops[i - 1].t, 1e-6f);
            const float f = (t - stops[i - 1].t) / span;
            return {stops[i - 1].r + (stops[i].r - stops[i - 1].r) * f,
                    stops[i - 1].g + (stops[i].g - stops[i - 1].g) * f,
                    stops[i - 1].b + (stops[i].b - stops[i - 1].b) * f, 1};
        }
    }
    return {stops[n - 1].r, stops[n - 1].g, stops[n - 1].b, 1};
}

// 黑体近似（Tanner Helland），T 单位 K，返回 0..1 的 sRGB。
inline jpov::Color BlackbodyRGB(float kelvin) {
    const float T = std::clamp(kelvin, 1000.0f, 12000.0f) / 100.0f;
    float r, g, b;
    if (T <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * std::log(T) - 161.1195681661f;
    } else {
        r = 329.698727446f * std::pow(T - 60.0f, -0.1332047592f);
        g = 288.1221695283f * std::pow(T - 60.0f, -0.0755148492f);
    }
    if (T >= 66.0f) {
        b = 255.0f;
    } else if (T <= 19.0f) {
        b = 0.0f;
    } else {
        b = 138.5177312231f * std::log(T - 10.0f) - 305.0447927307f;
    }
    return {std::clamp(r, 0.0f, 255.0f) / 255.0f,
            std::clamp(g, 0.0f, 255.0f) / 255.0f,
            std::clamp(b, 0.0f, 255.0f) / 255.0f, 1};
}

// tau（1=最热）→ 颜色。
inline jpov::Color FirePaletteColor(int palette, float tau) {
    tau = std::clamp(tau, 0.0f, 1.0f);
    if (palette == 0) {
        // 物理黑体：1200K（暗红）→ 6500K（白）
        const float kelvin = 1200.0f + tau * 5300.0f;
        jpov::Color c = BlackbodyRGB(kelvin);
        // 提亮一点，避免末段太暗（HDR）
        const float boost = 0.6f + 1.4f * tau;
        return {c.r * boost, c.g * boost, c.b * boost, 1};
    }
    if (palette == 1) {
        // 电影暖调：白热 → 黄 → 橙 → 深红
        static const PaletteStop kStops[] = {
            {0.00f, 0.10f, 0.010f, 0.000f},
            {0.25f, 0.70f, 0.090f, 0.010f},
            {0.50f, 1.20f, 0.380f, 0.040f},
            {0.75f, 1.55f, 0.820f, 0.180f},
            {1.00f, 1.80f, 1.550f, 1.150f},
        };
        return PaletteLerp(kStops, 5, tau);
    }
    // 2 Cute：粉紫白 → 蜜桃 → 暖橙 → 玫瑰暗红
    static const PaletteStop kCute[] = {
        {0.00f, 0.22f, 0.030f, 0.060f},
        {0.30f, 0.95f, 0.260f, 0.300f},
        {0.60f, 1.55f, 0.780f, 0.520f},
        {0.82f, 1.75f, 1.250f, 0.980f},
        {1.00f, 1.90f, 1.750f, 1.650f},
    };
    return PaletteLerp(kCute, 5, tau);
}

}  // namespace jpov_particle

#endif  // JPOV_DEMO_FIRE_PALETTE_H_

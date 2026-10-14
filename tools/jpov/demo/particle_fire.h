// JPOV 广告牌粒子火 — 查看器内的**程序化火焰 flipbook 生成器**（自包含、无外部资产）
//
// 生成一张 tiles_x×tiles_y 的火 flipbook（每格 cell×cell），编码成 PNG 字节，供
// JPOV::RegisterTextureFromMemory 上传。这样查看器不必依赖外部贴图文件即可验证
// 「flipbook 帧动画 + 广告牌粒子」这条链。真实项目应换成 Mantaflow 烘的序列帧
// （见 /james_pm/fire_hero/）。
//
// 2026-10-14 修（Danis 反馈「像薯片」）：① 旧版用 sin 合成噪声 → 规则条纹；
//   改为**哈希 value noise（2 阶 fbm）** → 有机、无周期纹路。② 旧版火苗填满格子、
//   贴到格子边界 → 广告牌四边露硬边；改为格内留**透明内边距（margin）**，让 alpha
//   在触边前归零 → 硬边消失。③ 每帧用不同噪声种子 + 低频横向摆动 → 帧间有演化。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。

#ifndef JPOV_DEMO_PARTICLE_FIRE_H_
#define JPOV_DEMO_PARTICLE_FIRE_H_

#include <cmath>
#include <cstdint>
#include <vector>

#include "third_party/stb/stb_image_write.h"

namespace jpov_particle_fire {

// 整数哈希 → [0,1)（确定性，无周期纹路）。f = 帧种子，使各帧纹理不同。
inline float Hash01(int x, int y, int f) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(f) * 2246822519u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

// 双线性 value noise（平滑插值）。
inline float ValueNoise(float x, float y, int f) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    const float a = Hash01(xi, yi, f);
    const float b = Hash01(xi + 1, yi, f);
    const float c = Hash01(xi, yi + 1, f);
    const float d = Hash01(xi + 1, yi + 1, f);
    const float top = a + (b - a) * u;
    const float bot = c + (d - c) * u;
    return top + (bot - top) * v;
}

// 2 阶 fbm（去规则纹路）。
inline float Fbm(float x, float y, int f) {
    return ValueNoise(x, y, f) * 0.62f +
           ValueNoise(x * 2.03f + 5.1f, y * 2.11f + 7.3f, f + 17) * 0.38f;
}

inline float SmoothStep01(float lo, float hi, float x) {
    if (hi <= lo) {
        return (x >= hi) ? 1.0f : 0.0f;
    }
    float t = (x - lo) / (hi - lo);
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// 生成火焰 flipbook 并编码为 PNG。
// 每格是一枚「下宽上收的有机火苗」：低频噪声摆动 + fbm 扰动边缘；颜色按温度梯度
// （亮芯近白黄、边橙红）；格内四周留透明内边距 → 广告牌不露硬边。
//
// Pre-condition: tiles_x>=1, tiles_y>=1, cell>=8。
inline std::vector<unsigned char> MakeFireFlipbookPng(int tiles_x, int tiles_y,
                                                      int cell, uint32_t seed) {
    const int width = tiles_x * cell;
    const int height = tiles_y * cell;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
    const int total = tiles_x * tiles_y;

    for (int f = 0; f < total; ++f) {
        const int ox = (f % tiles_x) * cell;
        const int oy = (f / tiles_x) * cell;
        const int fseed = f * 131 + static_cast<int>(seed);

        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                const float u = (x + 0.5f) / static_cast<float>(cell);  // 0..1 横向
                const float v = (y + 0.5f) / static_cast<float>(cell);  // 0=图像顶行
                const float fy = 1.0f - v;  // fy: 0=底, 1=顶（火苗朝上）

                // 低频横向摆动（顶部摆幅更大；用噪声，非正弦）。
                const float wob =
                    (Fbm(fy * 2.0f, static_cast<float>(f) * 0.7f, fseed) - 0.5f) *
                    0.22f * fy;
                // 宽度剖面：下宽上收。
                const float wprof = 0.36f * (1.0f - 0.72f * fy);
                const float cx = 0.5f + wob;
                const float dx = (u - cx) / wprof;

                // 主体形状 + 有机边缘扰动。
                const float yv = (fy - 0.15f) / 0.95f;
                float shape = 1.0f - (dx * dx + yv * yv * 0.6f);
                const float edge =
                    0.50f + 0.95f * Fbm(u * 4.0f, fy * 3.0f -
                                                    static_cast<float>(f) * 0.5f,
                                        fseed + 3);
                shape *= edge;
                shape = (shape < 0.0f) ? 0.0f : ((shape > 1.0f) ? 1.0f : shape);
                shape = std::pow(shape, 1.15f);
                shape *= (1.0f - 0.45f * fy);  // 顶部渐淡

                // 格内透明内边距（窄）：四边 alpha → 0（消除广告牌硬边）。
                const float ex = std::fmin(u, 1.0f - u);
                const float ey = std::fmin(v, 1.0f - v);
                const float margin = SmoothStep01(0.0f, 0.11f, std::fmin(ex, ey));

                float a = shape * margin;
                a = (a < 0.0f) ? 0.0f : ((a > 1.0f) ? 1.0f : a);

                // 温度梯度：亮芯近白黄、边橙红。RGB = 颜色（非预乘），A = 不透明度。
                const float rr = 1.0f;
                const float gg = 0.25f + 0.75f * std::fmin(1.0f, a * 1.25f);
                const float bb = a * a * a * 0.7f;
                const size_t idx =
                    (static_cast<size_t>(oy + y) * width + (ox + x)) * 4;
                pixels[idx + 0] = static_cast<unsigned char>(rr * 255.0f + 0.5f);
                pixels[idx + 1] = static_cast<unsigned char>(gg * 255.0f + 0.5f);
                pixels[idx + 2] = static_cast<unsigned char>(bb * 255.0f + 0.5f);
                pixels[idx + 3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
            }
        }
    }

    // 用 stb 的「输出到回调」接口把 PNG 写进内存 vector（_to_mem 非公开）。
    std::vector<unsigned char> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            std::vector<unsigned char>* vec =
                static_cast<std::vector<unsigned char>*>(ctx);
            const unsigned char* p = static_cast<const unsigned char*>(data);
            vec->insert(vec->end(), p, p + size);
        },
        &out, width, height, 4, pixels.data(), width * 4);
    return out;
}

}  // namespace jpov_particle_fire

#endif  // JPOV_DEMO_PARTICLE_FIRE_H_

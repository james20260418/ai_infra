// JPOV 广告牌粒子火 — 查看器内的**程序化火焰 flipbook 生成器**（自包含、无外部资产）
//
// 生成一张 tiles_x×tiles_y 的火 flipbook（每格 cell×cell），编码成 PNG 字节，供
// JPOV::RegisterTextureFromMemory 上传。这样查看器不必依赖外部贴图文件即可验证
// 「flipbook 帧动画 + 广告牌粒子」这条链。真实项目应换成 Mantaflow 烘的序列帧
// （见 /james_pm/fire_hero/）。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。

#ifndef JPOV_DEMO_PARTICLE_FIRE_H_
#define JPOV_DEMO_PARTICLE_FIRE_H_

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "third_party/stb/stb_image_write.h"

namespace jpov_particle_fire {

// 生成火焰 flipbook 并编码为 PNG。每格是一枚「泪滴状火苗」：底部窄、中部宽、
// 顶部收尖，带随帧推进的横向摆动与噪声纹理；颜色按温度梯度（亮芯近白黄、边缘橙红）。
// 返回 PNG 字节（可直接给 RegisterTextureFromMemory）。
//
// Pre-condition: tiles_x>=1, tiles_y>=1, cell>=4。
inline std::vector<unsigned char> MakeFireFlipbookPng(int tiles_x, int tiles_y,
                                                      int cell, uint32_t seed) {
    const int width = tiles_x * cell;
    const int height = tiles_y * cell;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
    const int total = tiles_x * tiles_y;

    for (int f = 0; f < total; ++f) {
        const int ox = (f % tiles_x) * cell;
        const int oy = (f / tiles_x) * cell;
        const float phase = static_cast<float>(f) / static_cast<float>(total);
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                const float u = (x + 0.5f) / static_cast<float>(cell);  // 0..1 横向
                const float v = (y + 0.5f) / static_cast<float>(cell);  // 0=图像顶行
                const float fy = 1.0f - v;  // fy: 0=底, 1=顶

                // 横向摆动（随帧变）+ 宽度剖面（中部宽）。
                const float wobble =
                    0.07f * std::sin(fy * 5.0f + phase * 6.2831853f * 2.0f + seed * 0.01f);
                const float cx = 0.5f + wobble;
                const float halfw =
                    0.30f * (0.45f + 0.95f * std::sin(3.14159265f * (0.22f + 0.62f * fy)));
                const float dx = (u - cx) / halfw;
                const float vy = (fy - 0.35f) / 0.80f;
                float a = 1.0f - (dx * dx + vy * vy * 0.55f);
                if (a < 0.0f) {
                    a = 0.0f;
                }
                a = std::pow(a, 2.2f);
                // 噪声（随位置 + 帧变化，制造火苗纹理）。
                const float n = 0.5f + 0.5f *
                    std::sin((u * 6.0f + fy * 4.0f + phase * 13.0f) * 3.3f);
                a *= (0.65f + 0.5f * n);
                if (a > 1.0f) {
                    a = 1.0f;
                }

                // 温度梯度：亮芯近白黄、边缘橙红。RGB = 颜色（非预乘），A = 不透明度。
                const float rr = 1.0f;
                const float gg = 0.28f + 0.68f * std::min(1.0f, a * 1.3f);
                const float bb = a * a * a * 0.85f;
                const size_t idx =
                    (static_cast<size_t>(oy + y) * width + (ox + x)) * 4;
                pixels[idx + 0] = static_cast<unsigned char>(std::min(1.0f, rr) * 255.0f + 0.5f);
                pixels[idx + 1] = static_cast<unsigned char>(std::min(1.0f, gg) * 255.0f + 0.5f);
                pixels[idx + 2] = static_cast<unsigned char>(std::min(1.0f, bb) * 255.0f + 0.5f);
                pixels[idx + 3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
            }
        }
    }

    // 用 stb 的“输出到回调”接口把 PNG 写进内存 vector（_to_mem 非公开）。
    std::vector<unsigned char> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            std::vector<unsigned char>* v =
                static_cast<std::vector<unsigned char>*>(ctx);
            const unsigned char* p = static_cast<const unsigned char*>(data);
            v->insert(v->end(), p, p + size);
        },
        &out, width, height, 4, pixels.data(), width * 4);
    return out;
}

}  // namespace jpov_particle_fire

#endif  // JPOV_DEMO_PARTICLE_FIRE_H_

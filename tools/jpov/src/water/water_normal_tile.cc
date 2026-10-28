// JPOV 波浪法线图集生成（CPU 烘 → GPU 纹理数组 + mip）。见 water_normal_tile.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_normal_tile.h"

#include <cmath>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// 八度表：**整数波数**（nx, nz）⇒ 空间可平铺；cycles = 一个循环里走几圈相位 ⇒ 时间可循环。
// 权重之和 = 1；斜率幅度最后统一归一化到 [-1, 1] 再存进 RGBA8。
struct Octave {
    int nx;
    int nz;
    float weight;
    int cycles;
};

constexpr Octave kOctaves[] = {
    {  5,   3, 0.22f, 1},
    {  3,  -7, 0.17f, 2},
    { 11,   2, 0.14f, 3},
    { -8,  13, 0.10f, 5},
    { 17,   5, 0.12f, 4},
    {  2, -19, 0.15f, 7},
    { 23,  11, 0.10f, 6},
};
constexpr int kOctaveCount = static_cast<int>(sizeof(kOctaves) / sizeof(kOctaves[0]));

}  // namespace

WaterNormalTile BuildWaterNormalTile(int size, int frames, float tile_m) {
    CHECK_GT(size, 0);
    CHECK_GT(frames, 0);
    CHECK_GT(tile_m, 0.0f);

    // 1) CPU：逐帧逐像素算斜率（-∇h），先写 float，最后统一归一化。
    const size_t pixels_per_frame = static_cast<size_t>(size) * static_cast<size_t>(size);
    std::vector<float> slope_x(pixels_per_frame * static_cast<size_t>(frames));
    std::vector<float> slope_z(pixels_per_frame * static_cast<size_t>(frames));
    float max_slope = 1.0e-6f;

    for (int f = 0; f < frames; ++f) {
        const float frame_t = static_cast<float>(f) / static_cast<float>(frames);
        for (int j = 0; j < size; ++j) {
            // tile 空间坐标 [0,1)：用 (i+0.5)/size 保证采样点不落在边界上。
            const float z = (static_cast<float>(j) + 0.5f) / static_cast<float>(size);
            for (int i = 0; i < size; ++i) {
                const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(size);
                float gx = 0.0f;
                float gz = 0.0f;
                for (int o = 0; o < kOctaveCount; ++o) {
                    const Octave& oc = kOctaves[o];
                    // 相位 = 2π(nx·x + nz·z) + 整数圈时间相位。
                    const float phase = kTwoPi * (static_cast<float>(oc.nx) * x +
                                                  static_cast<float>(oc.nz) * z) +
                                        kTwoPi * static_cast<float>(oc.cycles) * frame_t;
                    const float c = std::cos(phase) * oc.weight * kTwoPi;
                    // d/dx sin(2π(nx·x+nz·z)) = 2π·nx·cos(...) ⇒ 斜率按 tile 尺寸换算成 1/m。
                    gx += c * static_cast<float>(oc.nx) / tile_m;
                    gz += c * static_cast<float>(oc.nz) / tile_m;
                }
                const size_t idx = static_cast<size_t>(f) * pixels_per_frame +
                                   static_cast<size_t>(j) * static_cast<size_t>(size) +
                                   static_cast<size_t>(i);
                slope_x[idx] = gx;
                slope_z[idx] = gz;
                max_slope = std::max(max_slope, std::max(std::fabs(gx), std::fabs(gz)));
            }
        }
    }

    // 2) 归一化到 [-1,1] 并编码进 RGBA8：RG = 斜率（0.5 偏移），B = 1（预留方差/AO），A = 1。
    //    归一化是**必须**的：滑条控制的是"最大斜率"，纹理只提供"形状"。
    std::vector<unsigned char> texels(pixels_per_frame * static_cast<size_t>(frames) * 4u);
    const float inv = 1.0f / max_slope;
    for (size_t k = 0; k < pixels_per_frame * static_cast<size_t>(frames); ++k) {
        const float nx = slope_x[k] * inv;                 // [-1,1]
        const float nz = slope_z[k] * inv;
        texels[k * 4 + 0] = static_cast<unsigned char>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
        texels[k * 4 + 1] = static_cast<unsigned char>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
        texels[k * 4 + 2] = 255;
        texels[k * 4 + 3] = 255;
    }

    // 3) GPU：纹理数组（每层一帧），再让 GL 生成 mip 链。
    WaterNormalTile tile;
    tile.size = size;
    tile.frames = frames;
    glGenTextures(1, &tile.tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tile.tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, frames, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texels.data());
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
    // 掠射角下水面在屏幕上被压扁，各向异性过滤能显著减轻"横向硬条纹"。
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
#endif
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    LOG(INFO) << "water normal tile: " << size << "² × " << frames
              << " frames, tile=" << tile_m << " m, baked max slope=" << max_slope;
    return tile;
}

void ReleaseWaterNormalTile(WaterNormalTile* tile) {
    CHECK(tile != nullptr);
    if (tile->tex != 0) {
        glDeleteTextures(1, &tile->tex);
        tile->tex = 0;
    }
    tile->size = 0;
    tile->frames = 0;
}

}  // namespace jpov

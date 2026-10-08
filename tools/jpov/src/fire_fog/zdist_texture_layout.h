// JPOV Fire-Fog — ZDist 的 GPU 纹理布局（CPU 打包 / 解包）
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §16.3。
//
// 每个像素的 ZDist = 8 个控制点，每个控制点 5 个量 (z, τd, Ed.r, Ed.g, Ed.b)
// ⇒ 40 个 float。**关键：8 个 z 断点被 τd 与 Ed.rgb 四条通道共享**（同一组断点），
// 所以 z 只存一份，不是四份。若按「每条通道各存 (z,值)」会变成 8×2×4 = 64 float
// （= Danis 担心的「16 个纹理」）；共享断点后压到 40 float（= 10 个 RGBA32F texel）。
//
// 布局（SoA，单缓冲区 / 单纹理数组，stride = 10 texel/pixel）：
//   float[0..8)   : z0..z7              （2 texel）
//   float[8..16)  : τd0..τd7            （2 texel）
//   float[16..40) : Ed[0..7].rgb        （6 texel，逐点 r,g,b）
//   ⇒ 每像素 10 个 RGBA32F texel（40 float）。
//
// 更省的方向（本 PR 不实现，记档）：
//   - 全用 half（GL_RGBA16F）：texel 数不变（10），字节数减半；
//   - 控制点降到 ≤6（设计允许）：30 float ≈ 8 texel；
//   - level-sampled（设计文档 §14.9）：τd 存固定 8 档对应 z、τ_total 单存，语义等价、
//     z 与 τ 合一（省 7 float），但改表示、末积分要按档重建 —— 留后续。

#ifndef JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_
#define JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_

#include <cstddef>

#include "tools/jpov/src/fire_fog/zdist_function.h"

namespace jpov {

struct ZDistTextureLayout {
    static constexpr int kPoints = kZDistControlPoints;         // 8 控制点
    static constexpr int kFloatsPerPixel = kPoints * 5;         // 40 = 8×(z,τd,Ed.rgb)
    static constexpr int kTexelsPerPixel = kFloatsPerPixel / 4;  // 10 个 RGBA32F texel

    static constexpr int kFloatOffsetZ = 0;    // [0, 8)   z0..z7
    static constexpr int kFloatOffsetTau = 8;  // [8, 16)  τd0..τd7
    static constexpr int kFloatOffsetEd = 16;  // [16, 40) Ed[0..7].rgb
};

// 把压缩后的 ZDist 打包进 out（长度 kFloatsPerPixel）。不足 8 点时，用最后一点填充
// 剩余槽位（值相同、z 相同），shader 侧固定按 8 槽读。
// Pre-condition: out != nullptr；f.size() ∈ [2, 8]
inline void PackZDist(const ZDistFunction& f, float* out /*output*/) {
    CHECK(out != nullptr);
    const int n = f.size();
    CHECK_GE(n, 2);
    CHECK_LE(n, ZDistTextureLayout::kPoints);
    for (int i = 0; i < ZDistTextureLayout::kPoints; ++i) {
        const int s = (i < n) ? i : n - 1;  // 越界槽位复制最后一点
        out[ZDistTextureLayout::kFloatOffsetZ + i] =
            static_cast<float>(f.z(s));
        out[ZDistTextureLayout::kFloatOffsetTau + i] =
            static_cast<float>(f.tau(s));
        const Vec3f e = f.ed(s);
        out[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 0] = e[0];
        out[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 1] = e[1];
        out[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 2] = e[2];
    }
}

// 从打包缓冲解包出 ZDistFunction（末尾重复 z 的填充槽位会被丢弃）。
// Pre-condition: in != nullptr
inline ZDistFunction UnpackZDist(const float* in) {
    CHECK(in != nullptr);
    double zs[ZDistTextureLayout::kPoints];
    double taus[ZDistTextureLayout::kPoints];
    Vec3f eds[ZDistTextureLayout::kPoints];
    int n = 0;
    for (int i = 0; i < ZDistTextureLayout::kPoints; ++i) {
        const double z =
            static_cast<double>(in[ZDistTextureLayout::kFloatOffsetZ + i]);
        if (i > 0 && z <= zs[n - 1]) {
            break;  // 填充槽位（z 不再严格递增）→ 停止
        }
        zs[n] = z;
        taus[n] =
            static_cast<double>(in[ZDistTextureLayout::kFloatOffsetTau + i]);
        eds[n] = Vec3f(in[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 0],
                       in[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 1],
                       in[ZDistTextureLayout::kFloatOffsetEd + i * 3 + 2]);
        ++n;
    }
    CHECK_GE(n, 2) << "解包后控制点不足 2 个";
    return ZDistFunction::FromSamples(zs, taus, eds, n);
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_

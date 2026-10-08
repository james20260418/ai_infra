// JPOV Fire-Fog — ZDist 的 GPU 纹理布局（CPU 打包 / 解包）
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §11.5。
//
// 每个像素的 ZDist = 8 个控制点，每点 5 个量 (z, τd, Ed.r, Ed.g, Ed.b)。经精度分析
// （§11.6）后定下的格式：
//   - z  : **fp32**（必须 —— 要分辨相邻断点的小间隙、且与场景深度对齐；颜色积分本身不含 z）；
//   - τd : **uint16**，归一化到固定量程 [0, kTauMax]（τ 进指数 ⇒ 要**绝对**精度，unorm 远胜 fp16）；
//   - Ed : **fp16**（线性进末端积分 ⇒ 相对精度足够；HDR 也在 half 量程内）。
//
// 布局 = **6 个 RGBA32F texel / 像素**（= 24 个 32-bit lane / 像素，SoA、单纹理数组）：
//   lane[0..8)    : z0..z7            （fp32，8 lane = 2 texel）
//   lane[8..12)   : τd0..τd7          （uint16×8，2/lane，4 lane = 1 texel）
//   lane[12..24)  : Ed[0..7].rgb      （fp16×24，2/lane，12 lane = 3 texel）
//
// 对比：若按「每条通道各存 (z,值)」= 64 float = 16 个 RGBA32F（Danis 担心的「16 纹理」）；
// 共享 z + 混合精度后 = 6 个 RGBA32F（96 字节/像素，比纯 fp32 的 10 texel 省 ~40%）。

#ifndef JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_
#define JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_

#include <cmath>
#include <cstdint>
#include <cstring>

#include "tools/jpov/src/fire_fog/zdist_function.h"

namespace jpov {

struct ZDistTextureLayout {
    static constexpr int kPoints = kZDistControlPoints;  // 8 控制点
    static constexpr int kTexelsPerPixel = 6;            // RGBA32F texel 数 / 像素
    static constexpr int kLanesPerPixel = kTexelsPerPixel * 4;  // 24 个 32-bit lane
    static constexpr int kBytesPerPixel = kLanesPerPixel * 4;   // 96 字节

    static constexpr int kLaneOffsetZ = 0;    // [0, 8)   z fp32
    static constexpr int kLaneOffsetTau = 8;  // [8, 12)  τd uint16（2/lane）
    static constexpr int kLaneOffsetEd = 12;  // [12, 24) Ed fp16（2/lane）

    // τd 归一化量程：τ > kTauMax 时 T = exp(−τ) < e^-32 ≈ 1e-14，视觉等价于 0，
    // 故固定量程即可（无需每像素存 τ_max）。步长 = kTauMax / 65535 ≈ 4.9e-4（绝对）。
    static constexpr float kTauMax = 32.0f;
};

namespace zdist_pack_detail {

// float → IEEE binary16 位模式（round-to-nearest-even）。便携实现（不依赖 _Float16，
// 以兼容 Windows/MinGW 交叉编译）。
inline uint16_t FloatToHalfBits(float value) {
    uint32_t x;
    std::memcpy(&x, &value, sizeof(x));
    const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
    const int exponent = static_cast<int>((x >> 23) & 0xffu) - 127 + 15;
    const uint32_t mantissa = x & 0x007fffffu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return sign;  // 太小 → 0
        }
        uint32_t m = mantissa | 0x00800000u;  // 隐含 1
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        const uint32_t rounded =
            (m + (1u << (shift - 1)) - 1u + ((m >> shift) & 1u)) >> shift;
        return static_cast<uint16_t>(sign | rounded);
    }
    if (exponent >= 31) {
        if (((x >> 23) & 0xffu) == 0xffu && mantissa != 0) {
            return static_cast<uint16_t>(sign | 0x7c00u | 0x0200u);  // NaN
        }
        return static_cast<uint16_t>(sign | 0x7c00u);  // inf / 溢出
    }
    const uint32_t rounded =
        (mantissa + 0x00000fffu + ((mantissa >> 13) & 1u)) >> 13;
    // rounded 进位会自然冒进 exponent（exp<<10 与 0x400 相加）。
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 rounded);
}

// IEEE binary16 位模式 → float。
inline float HalfBitsToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t man = h & 0x3ffu;
    uint32_t out = 0;
    if (exp == 0) {
        if (man == 0) {
            out = sign;  // ±0
        } else {
            int e = -1;  // 子正规 → 规格化
            while ((man & 0x400u) == 0) {
                man <<= 1;
                ++e;
            }
            man &= 0x3ffu;
            out = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | (man << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7f800000u | (man << 13);  // inf / nan
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

inline uint16_t EncodeTau(double tau) {
    double q = tau / static_cast<double>(ZDistTextureLayout::kTauMax) * 65535.0;
    if (q < 0.0) {
        q = 0.0;
    }
    if (q > 65535.0) {
        q = 65535.0;
    }
    return static_cast<uint16_t>(std::lround(q));
}
inline double DecodeTau(uint16_t q) {
    return static_cast<double>(q) / 65535.0 *
           static_cast<double>(ZDistTextureLayout::kTauMax);
}

// 把 16-bit 值塞进 lane 的低/高半（pair = 0/1）。
inline void SetHalfLane(uint32_t* lanes, int lane, int pair, uint16_t v) {
    if (pair == 0) {
        lanes[lane] = (lanes[lane] & 0xffff0000u) | static_cast<uint32_t>(v);
    } else {
        lanes[lane] = (lanes[lane] & 0x0000ffffu) | (static_cast<uint32_t>(v) << 16);
    }
}
inline uint16_t GetHalfLane(const uint32_t* lanes, int lane, int pair) {
    return static_cast<uint16_t>(pair == 0
                                     ? (lanes[lane] & 0xffffu)
                                     : ((lanes[lane] >> 16) & 0xffffu));
}

}  // namespace zdist_pack_detail

// 打包 ZDist 到 out（kLanesPerPixel 个 32-bit lane = 6 个 RGBA32F texel）。
// 不足 8 点时用最后一点填充剩余槽位（shader 侧固定按 8 槽读）。
// Pre-condition: out != nullptr；f.size() ∈ [2, 8]
inline void PackZDist(const ZDistFunction& f, uint32_t* out /*output*/) {
    CHECK(out != nullptr);
    const int n = f.size();
    CHECK_GE(n, 2);
    CHECK_LE(n, ZDistTextureLayout::kPoints);
    for (int i = 0; i < ZDistTextureLayout::kLanesPerPixel; ++i) {
        out[i] = 0u;  // 先清零（半字写入不再处理未初始化高/低位）
    }
    for (int i = 0; i < ZDistTextureLayout::kPoints; ++i) {
        const int s = (i < n) ? i : n - 1;  // 越界槽位复制最后一点
        // z（fp32 位模式）
        const float zf = static_cast<float>(f.z(s));
        std::memcpy(&out[ZDistTextureLayout::kLaneOffsetZ + i], &zf, sizeof(zf));
        // τd（uint16）
        const uint16_t qt = zdist_pack_detail::EncodeTau(f.tau(s));
        zdist_pack_detail::SetHalfLane(out, ZDistTextureLayout::kLaneOffsetTau + i / 2,
                                       i % 2, qt);
        // Ed.rgb（fp16）
        const Vec3f e = f.ed(s);
        for (int c = 0; c < 3; ++c) {
            const int j = i * 3 + c;
            const uint16_t qe = zdist_pack_detail::FloatToHalfBits(e[c]);
            zdist_pack_detail::SetHalfLane(out, ZDistTextureLayout::kLaneOffsetEd + j / 2,
                                           j % 2, qe);
        }
    }
}

// 从打包 lane 解包出 ZDistFunction（末尾重复 z 的填充槽位会被丢弃）。
// Pre-condition: in != nullptr
inline ZDistFunction UnpackZDist(const uint32_t* in) {
    CHECK(in != nullptr);
    double zs[ZDistTextureLayout::kPoints];
    double taus[ZDistTextureLayout::kPoints];
    Vec3f eds[ZDistTextureLayout::kPoints];
    int n = 0;
    for (int i = 0; i < ZDistTextureLayout::kPoints; ++i) {
        float zf;
        std::memcpy(&zf, &in[ZDistTextureLayout::kLaneOffsetZ + i], sizeof(zf));
        const double z = static_cast<double>(zf);
        if (i > 0 && z <= zs[n - 1]) {
            break;  // 填充槽位（z 不再严格递增）→ 停止
        }
        zs[n] = z;
        taus[n] = zdist_pack_detail::DecodeTau(
            zdist_pack_detail::GetHalfLane(in, ZDistTextureLayout::kLaneOffsetTau + i / 2,
                                           i % 2));
        Vec3f e(0.0f, 0.0f, 0.0f);
        for (int c = 0; c < 3; ++c) {
            const int j = i * 3 + c;
            e[c] = zdist_pack_detail::HalfBitsToFloat(
                zdist_pack_detail::GetHalfLane(in, ZDistTextureLayout::kLaneOffsetEd + j / 2,
                                               j % 2));
        }
        eds[n] = e;
        ++n;
    }
    CHECK_GE(n, 2) << "解包后控制点不足 2 个";
    return ZDistFunction::FromSamples(zs, taus, eds, n);
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_ZDIST_TEXTURE_LAYOUT_H_

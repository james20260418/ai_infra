// JPOV postprocess — sRGB 编解码（IEC 61966-2-1）。
//
// 这是 sRGB 编码/解码的 C++ 参考实现，与 renderer.cc tone map shader 里的
// GLSL srgb_encode_channel() / SrgbToLinear() 公式**逐字一致**（单一公式来源）。
//
// 目的：给 postprocess 的数学查表 test（srgb_math_test）提供可链接的实现，
// 用 IEC 61966-2-1 标准查表值做确定性验证——捕获"公式写错、分段边界错、
// clamp 错"这类 bug。这是渲染链路端到端验证（srgb_encode_anchor_test）的
// 纯函数互补。
//
// 若修改此处公式，必须同步修改：
//   tools/jpov/src/renderer.cc  kTonemapFs 里的 srgb_encode_channel / srgb_encode
//   tools/jpov/src/renderer.cc  anonymous namespace 里的 SrgbToLinear
#pragma once

#include <cmath>

namespace jpov {
namespace postprocess {

// 线性 → sRGB 编码（IEC 61966-2-1）。输入 [0,1] 线性值 → 输出 [0,1] sRGB 值。
//   分段：x<=0.0031308 线性段；x>0.0031308 幂段（指数 1/2.4）。
//   与 renderer GLSL srgb_encode_channel() 一致。
inline float LinearToSrgb(float x) {
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

// sRGB → 线性 解码（IEC 61966-2-1 逆变换）。输入 [0,1] sRGB 值 → 输出 [0,1] 线性值。
//   分段：x<=0.04045 线性段；x>0.04045 幂段（指数 2.4）。
//   与 renderer anonymous namespace 的 SrgbToLinear() 一致。
inline float SrgbToLinear(float x) {
    if (x <= 0.04045f) {
        return x / 12.92f;
    }
    return std::pow((x + 0.055f) / 1.055f, 2.4f);
}

}  // namespace postprocess
}  // namespace jpov

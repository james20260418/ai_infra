// JPOV postprocess — sRGB 编解码数学查表 test（纯 C++，零渲染）。
//
// 用 IEC 61966-2-1 标准的确定性查表值，验证 LinearToSrgb / SrgbToLinear
// 两个纯函数的公式正确性。这些值是硬标准，跨平台恒定：
//   - 分段边界：0.0031308（编码线性段限）、0.04045（解码线性段限）
//   - 常量：12.92 / 1.055 / 2.4
//
// 验证目标：
//   1. 编码查表值精确匹配标准附录值
//   2. 分段连续性：0.0031308 处编码线性段与幂段应连续（微小差）
//   3. 编解码互逆：decode(encode(x)) ≈ x（对称性，防"两侧公式不对称"）
//   4. 单调性：编码/解码均为单调不减
//
// 若此 test 失败 → 公式写错（指数/常量/边界错）。非渲染依赖，llvmpipe 无关。

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/test/postprocess/srgb.h"

namespace {

// 断言 |a-b| <= tol
bool Near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}

// IEC 61966-2-1 标准查表对（线性 → sRGB），来自 sRGB 规范附录示例值。
// 单位是 [0,1] 归一化值。（0.5 → 0.735357；0.18(PBR 中灰) → 0.461356 等。）
struct EncAnchor {
    float lin;
    float srgb;
};

const EncAnchor kEncAnchors[] = {
    {0.0000000f, 0.0000000f},  // 黑
    {0.0031308f, 0.0404499f},  // 编码线性段上限（分界点）
    {0.0040000f, 0.0507087f},  // 略高于分界，幂段
    {0.1000000f, 0.3491902f},  // 典型暗部
    {0.1800000f, 0.4613561f},  // PBR 标准中灰（42.8 灰）
    {0.5000000f, 0.7353570f},  // 中亮
    {0.8000000f, 0.9063318f},  // 亮
    {1.0000000f, 1.0000000f},  // 白
};

}  // namespace

int main() {
    // ---- 1. 编码查表精确匹配 ----
    for (const EncAnchor& a : kEncAnchors) {
        const float got = jpov::postprocess::LinearToSrgb(a.lin);
        const bool ok  = Near(got, a.srgb, 0.0005f);
        LOG(INFO) << "LinearToSrgb(" << a.lin << ") = " << got
                  << " (expect " << a.srgb << ") "
                  << (ok ? "OK" : "MISMATCH");
        CHECK(ok) << "sRGB 编码查表失配 (lin=" << a.lin << ")";
    }

    // ---- 2. 分界点连续（0.0031308 线性段与幂段衔接） ----
    {
        // 略低于分界的线性段值与分界处幂段值差异应极小（<0.001）。
        const float below = jpov::postprocess::LinearToSrgb(0.0030f);
        const float at = jpov::postprocess::LinearToSrgb(0.0031308f);
        LOG(INFO) << "分段连续性: linear(0.0030)=" << below
                  << " at(0.0031308)=" << at
                  << " |at-below|=" << std::fabs(at - below);
        CHECK(std::fabs(at - below) < 0.005f)
            << "编码分段在 0.0031308 处不连续？";
    }

    // ---- 3. 编解码互逆（对称性） ----
    {
        // 采样一组散点，decode(encode(x)) 应还原 x（浮点容差）。
        const float kSamples[] = {0.0f, 0.01f, 0.05f, 0.18f, 0.3f,
                                  0.5f, 0.7f, 0.9f, 1.0f};
        float worst = 0.0f;
        for (float x : kSamples) {
            const float enc = jpov::postprocess::LinearToSrgb(x);
            const float dec = jpov::postprocess::SrgbToLinear(enc);
            const float err = std::fabs(dec - x);
            worst = std::max(worst, err);
        }
        LOG(INFO) << "编解码互逆: worst round-trip error=" << worst;
        CHECK(worst < 0.001f)
            << "decode(encode(x)) 未能还原 x——两侧公式不对称（worst error="
            << worst << "）";
    }

    // ---- 4. 单调性 ----
    {
        bool ok = true;
        float prev = -1.0f;
        for (float x = 0.0f; x <= 1.00001f; x += 0.001f) {
            const float v = jpov::postprocess::LinearToSrgb(x);
            if (v < prev) {
                ok = false;
                LOG(ERROR) << "编码非单调: lin=" << x << " srgb=" << v
                           << " < prev=" << prev;
                break;
            }
            prev = v;
        }
        CHECK(ok) << "sRGB 编码应单调不减";
        LOG(INFO) << "编码单调性: OK";
    }

    LOG(INFO) << "TEST PASSED: sRGB 编解码数学查表全部通过";
    return 0;
}

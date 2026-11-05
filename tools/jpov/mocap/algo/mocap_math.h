// JPOV Mocap — 算法/拟合共用的小数学工具（header-only, GL-free）。
//
// 只放「无状态、可单测」的纯函数：中位数、零相位高斯、向量归一。算法与 fit 都依赖
// 同一份实现 —— 否则「lift2d_t 的平滑」与「root 的平滑」会慢慢长成两个不同的滤波器，
// 批量比对时说不清差异来自算法还是来自滤波。

#ifndef JPOV_MOCAP_ALGO_MOCAP_MATH_H_
#define JPOV_MOCAP_ALGO_MOCAP_MATH_H_

#include <algorithm>
#include <cmath>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"

namespace jpov {
namespace mocap {
namespace internal {

inline constexpr float kMocapEps = 1e-6f;

inline float Median(std::vector<float> values) {
    if (values.empty()) {
        return 0.0f;
    }
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<long>(mid), values.end());
    const float hi = values[mid];
    if (values.size() % 2 == 1) {
        return hi;
    }
    const float lo = *std::max_element(values.begin(), values.begin() + static_cast<long>(mid));
    return 0.5f * (lo + hi);
}

inline std::vector<float> GaussianKernel(int window) {
    CHECK_GE(window, 1) << "GaussianKernel: window 必须 >=1，收到 " << window;
    if (window % 2 == 0) {
        ++window;
    }
    const int radius = window / 2;
    const float sigma = std::max(1.0f, static_cast<float>(window) / 6.0f);
    std::vector<float> kernel(static_cast<size_t>(window));
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float t = static_cast<float>(i) / sigma;
        const float v = std::exp(-0.5f * t * t);
        kernel[static_cast<size_t>(i + radius)] = v;
        sum += v;
    }
    CHECK_GT(sum, 0.0f);
    for (float& v : kernel) {
        v /= sum;
    }
    return kernel;
}

// 零相位高斯：正反各滤一次，相位抵消（没有普通滑动平均的 (win−1)/2 帧滞后）。
inline std::vector<float> SmoothZeroPhase(const std::vector<float>& values, int window) {
    if (values.size() < 3 || window <= 1) {
        return values;
    }
    const std::vector<float> kernel = GaussianKernel(window);
    const int radius = static_cast<int>(kernel.size()) / 2;
    auto forward = [&](const std::vector<float>& in) {
        std::vector<float> out(in.size(), 0.0f);
        for (size_t i = 0; i < in.size(); ++i) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int idx = static_cast<int>(i) + k;
                idx = std::max(0, std::min(static_cast<int>(in.size()) - 1, idx));
                acc += kernel[static_cast<size_t>(k + radius)] * in[static_cast<size_t>(idx)];
            }
            out[i] = acc;
        }
        return out;
    };
    const std::vector<float> pass1 = forward(values);
    std::vector<float> reversed(pass1.rbegin(), pass1.rend());
    const std::vector<float> pass2 = forward(reversed);
    return std::vector<float>(pass2.rbegin(), pass2.rend());
}

inline std::vector<float> MedianFilter(const std::vector<float>& values, int window) {
    if (values.size() < 3 || window < 3) {
        return values;
    }
    const int radius = window / 2;
    std::vector<float> out(values.size(), 0.0f);
    for (size_t i = 0; i < values.size(); ++i) {
        std::vector<float> local;
        local.reserve(static_cast<size_t>(2 * radius + 1));
        for (int k = -radius; k <= radius; ++k) {
            int idx = static_cast<int>(i) + k;
            idx = std::max(0, std::min(static_cast<int>(values.size()) - 1, idx));
            local.push_back(values[static_cast<size_t>(idx)]);
        }
        out[i] = Median(std::move(local));
    }
    return out;
}

// 归一化；长度过小/非有限 → 清零并返回 false（调用方决定回落到 rest）。
inline bool NormalizeVec3(geom::Vec3<float>* v) {
    CHECK(v != nullptr) << "NormalizeVec3: v 不能为空";
    const float len = v->Norm();
    if (!std::isfinite(len) || len < kMocapEps) {
        *v = geom::Vec3<float>{};
        return false;
    }
    *v = *v * (1.0f / len);
    return true;
}

inline float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

}  // namespace internal
}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_MATH_H_

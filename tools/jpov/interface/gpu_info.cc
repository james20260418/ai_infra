#include "tools/jpov/interface/gpu_info.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace jpov {
namespace {

// 已知的**软件光栅器**名字（小写比对）。命中即 kCpuRaster。
//
// 收录依据（都是各家自己写在 GL_RENDERER 里的稳定字符串）：
//   llvmpipe        —— Mesa 的多线程 CPU 光栅器（WSL / 容器里最常见）
//   softpipe        —— Mesa 的参考实现 CPU 光栅器
//   swrast          —— Mesa 老的软件光栅器
//   software rasterizer / software renderer —— Apple 的软件渲染器（"Apple Software Renderer"）
//   gdi generic     —— Windows 无显卡驱动时的 Microsoft 软件 GL（GL 1.1）
//   microsoft basic render —— Windows 的软件 D3D 适配器（转译到 GL 时偶见）
constexpr const char* kSoftwareRendererNames[] = {
    "llvmpipe",
    "softpipe",
    "swrast",
    "software rasterizer",
    "software renderer",
    "gdi generic",
    "microsoft basic render",
};

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

const char* GpuKindName(GpuKind kind) {
    switch (kind) {
        case GpuKind::kUnknown:
            return "unknown";
        case GpuKind::kHardware:
            return "hardware";
        case GpuKind::kCpuRaster:
            return "cpu-raster";
    }
    // 非法枚举值（越界转换）：不静默返回某个类别，直接报 unknown 并让调用方看到原值。
    return "invalid";
}

GpuKind ClassifyGpuRenderer(const std::string& renderer) {
    if (renderer.empty()) {
        return GpuKind::kUnknown;
    }
    const std::string lower = ToLower(renderer);
    for (const char* name : kSoftwareRendererNames) {
        if (lower.find(name) != std::string::npos) {
            return GpuKind::kCpuRaster;
        }
    }
    // 不是已知软件光栅器 ⇒ 按硬件归类（语义见 GpuKind::kHardware 的注释：
    // 这是「不是已知软件光栅器」，不是「已确认是硬件」）。
    return GpuKind::kHardware;
}

}  // namespace jpov

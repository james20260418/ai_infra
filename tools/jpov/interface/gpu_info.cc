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
//   mesa offscreen  —— Mesa 的 EGL surfaceless / OSMesa 平台（无窗口的 CI 里常见）
//   software rasterizer / software renderer —— Apple 的软件渲染器（"Apple Software Renderer"）
//   swiftshader     —— Google 的 CPU 光栅器（Chrome/ANGLE、云 VM、远程桌面里很常见；
//                      串形如 "ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device ...))"）
//   gdi generic     —— Windows 无显卡驱动时的 Microsoft 软件 GL（GL 1.1）
//   microsoft basic render —— Windows 的软件 D3D 适配器（转译到 GL 时偶见）
//
// ⚠️ 漏一个名字的代价是**不对称**的：漏掉的软件光栅器会被归到 kHardware，
// 于是 HUD 在"最需要它说实话"的场合（云 VM / 远程桌面 / CI）说"hardware"，
// 以 kCpuRaster 为准的门禁也会放行。所以新增软件光栅器名字时**必须同时补测试**。
constexpr const char* kSoftwareRendererNames[] = {
    "llvmpipe",
    "softpipe",
    "swrast",
    "mesa offscreen",
    "software rasterizer",
    "software renderer",
    "swiftshader",
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

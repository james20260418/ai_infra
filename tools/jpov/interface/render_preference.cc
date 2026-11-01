// JPOV RenderPreference 实现（纯 CPU，不碰 GL）。

#include "tools/jpov/interface/render_preference.h"

#include <cstdlib>

namespace jpov {
namespace {

// Mesa 系驱动在**建 context 时**读这个变量；对已创建的 context 无效，
// 所以调用点必须在 glfwInit / glfwCreateWindow 之前。
constexpr const char* kSoftwareRasterEnvVar = "LIBGL_ALWAYS_SOFTWARE";

}  // namespace

const char* RenderPreferenceName(RenderPreference preference) {
    switch (preference) {
        case RenderPreference::kAny:
            return "any";
        case RenderPreference::kPreferGpu:
            return "prefer-gpu";
        case RenderPreference::kPreferCpu:
            return "prefer-cpu";
    }
    return "invalid";
}

RenderPreferenceVerdict JudgeRenderPreference(RenderPreference preference,
                                              GpuKind kind) {
    switch (preference) {
        case RenderPreference::kAny:
            return RenderPreferenceVerdict::kSatisfied;
        case RenderPreference::kPreferGpu:
            if (kind == GpuKind::kCpuRaster) {
                return RenderPreferenceVerdict::kViolated;
            }
            if (kind == GpuKind::kHardware) {
                return RenderPreferenceVerdict::kSatisfied;
            }
            return RenderPreferenceVerdict::kUndetermined;
        case RenderPreference::kPreferCpu:
            if (kind == GpuKind::kCpuRaster) {
                return RenderPreferenceVerdict::kSatisfied;
            }
            if (kind == GpuKind::kHardware) {
                return RenderPreferenceVerdict::kViolated;
            }
            return RenderPreferenceVerdict::kUndetermined;
    }
    return RenderPreferenceVerdict::kUndetermined;
}

bool ApplyRenderPreferenceEnvHint(RenderPreference preference) {
    if (preference != RenderPreference::kPreferCpu) {
        return false;
    }
    if (std::getenv(kSoftwareRasterEnvVar) != nullptr) {
        return false;
    }
#if defined(_WIN32)
    // MinGW / MSVC 的 CRT 版本（setenv 不在 Windows CRT 里）。
    return _putenv_s(kSoftwareRasterEnvVar, "1") == 0;
#else
    // overwrite = 0：即使并发中途被人设上也不覆盖（getenv 检查只是快速路径）。
    return setenv(kSoftwareRasterEnvVar, "1", /*overwrite=*/0) == 0;
#endif
}

}  // namespace jpov

// JPOV RenderPreference — 用户对「这一帧由谁来画」的偏好声明
//
// 为什么叫「偏好」不叫「选择」：Windows/Linux + OpenGL 这套栈里，**应用没有选设备
// 的能力**（核心 WGL 没有这个概念，WGL_NV_gpu_affinity 已废弃，GLFW 又不暴露 EGL
// device platform；详见 interface/gpu_info.h 顶部注释）。最终落到哪块设备由驱动、
// 系统「显示设置 → 图形」的 per-app 首选项、NVIDIA 控制面板、X server 决定。
//
// 所以这个偏好能做的只有三件事，而且都不保证结果：
//   1. 建 context **之前**给系统一点提示（见 ApplyRenderPreferenceEnvHint，
//      以及 Windows 侧 exe 导出表里的 NvOptimusEnablement /
//      AmdPowerXpressRequestHighPerformance）；
//   2. 建 context **之后**用 glGetString 的真话核对偏好是否被满足
//      （见 JudgeRenderPreference）；
//   3. 不满足时把「为什么」和「怎么办」写进日志，而不是让用户对着 1 fps 猜。
//
// 判定一律以 GpuKind::kCpuRaster 为准（唯一可确定的类别）：软件光栅器名字是
// Mesa / 微软自己写的、稳定可匹配；而「这是真硬件」无法从字符串反推。

#ifndef JPOV_RENDER_PREFERENCE_H_
#define JPOV_RENDER_PREFERENCE_H_

#include <cstdint>

#include "tools/jpov/interface/gpu_info.h"

namespace jpov {

// 渲染设备偏好。三态而非 bool：bool 只能表达「要/不要」，而「不关心」是默认且
// 最常见的状态（老 app 一行都不用改）。
enum class RenderPreference : uint8_t {
    // 不关心（默认）：不设提示、不判定、日志与 JPOV 老版本完全一致。
    kAny = 0,

    // 想要硬件光栅：Windows 上导出「请求高性能 GPU」符号（见 gpu_info.h 注释）；
    // 落到已知软件光栅器（llvmpipe / SwiftShader / GDI Generic / WARP …）时告警。
    kPreferGpu = 1,

    // 想要软件光栅：建 context 前对 Mesa 系驱动设 LIBGL_ALWAYS_SOFTWARE=1
    // （已存在同名环境变量时不覆盖）；落到 kHardware 时告警。
    // 用途：黄金图 / 像素基准要在**确定的**光栅器上跑，而不是有时 llvmpipe
    // 有时独显（两者的 MSAA / mip / 精度细节确实会差几个 LSB）。
    kPreferCpu = 2,
};

// 偏好被满足的程度。三态：kUnknown 的 GPU 判定**不能**当成违反
// （glGetString 拿不到字符串时我们什么都不知道，冤枉告警比不告警更糟）。
enum class RenderPreferenceVerdict : uint8_t {
    kSatisfied = 0,     // 明确满足
    kViolated = 1,      // 明确违反（可判定）
    kUndetermined = 2,  // GPU 类别未知（kUnknown）⇒ 无法判定
};

// 名字（ASCII，供日志用）：kAny → "any"、kPreferGpu → "prefer-gpu"、
// kPreferCpu → "prefer-cpu"。非法枚举值 → "invalid"。
const char* RenderPreferenceName(RenderPreference preference);

// 判定：这个 GPU 类别是否满足偏好（纯函数，不碰 GL / 环境变量，可单测）。
//
//   preference  | kCpuRaster        | kHardware                  | kUnknown
//   ------------|-------------------|----------------------------|----------------
//   kAny        | kSatisfied        | kSatisfied                 | kSatisfied
//   kPreferGpu  | kViolated         | kSatisfied                 | kUndetermined
//   kPreferCpu  | kSatisfied        | kViolated                  | kUndetermined
//
// 注意 kPreferCpu 那行：kHardware 的语义是「不是已知软件光栅器」，所以对
// 「想要软件光栅」来说它确实是违反（ANGLE / virgl / WARP 这类中间地带会被
// 归到这里 —— 它们同样不是 llvmpipe）。
RenderPreferenceVerdict JudgeRenderPreference(RenderPreference preference,
                                              GpuKind kind);

// 建 GL context **之前**调用：按偏好给驱动设环境变量提示。
//
//   kPreferCpu + LIBGL_ALWAYS_SOFTWARE 未设置 → 设为 "1"，返回 true（本次真写入）。
//   其他情况                                  → 不动环境，返回 false。
//
// 为什么「已存在就不覆盖」：用户在 shell / CI 里显式写的东西永远比引擎的默认偏好
// 更权威（`LIBGL_ALWAYS_SOFTWARE=0` 也是显式意图）。
// 为什么只对 Mesa 系有效：这个变量是 Mesa 的；Windows 上「系统 opengl32.dll +
// 厂商 ICD」那条栈里根本没有软件后端，此时调用方只能按判定结果告警。
bool ApplyRenderPreferenceEnvHint(RenderPreference preference);

}  // namespace jpov

#endif  // JPOV_RENDER_PREFERENCE_H_

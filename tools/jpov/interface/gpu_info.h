// JPOV GpuInfo — 当前渲染所用硬件/驱动的只读快照
//
// 为什么是「快照」而不是「可选设备清单」：
//   Windows + OpenGL 这套栈里**应用没有选择设备的能力** —— 核心 WGL 没有这个概念，
//   WGL_NV_gpu_affinity 已废弃；Linux 上要真选卡得走 EGL device platform
//   (eglQueryDevicesEXT / EGL_EXT_platform_device)，而 GLFW 不暴露那条路。
//   选卡的实际归属是：NVIDIA 控制面板的 per-profile 设置 / Windows「显示设置 →
//   图形」的 per-app 首选项 / X server 的配置。
//
//   所以 JPOV 做的是**准确报告「我这帧到底谁在画」**：glGetString 是 per-context
//   的真话，比「枚举 OS adapter」更接近事实（枚举出来的名字和实际渲染的设备可能对不上）。
//
// 采集时机：JPOV::Init() 创建 GL context 之后采一次，此后不变（JPOV 全进程单 context）。

#ifndef JPOV_GPU_INFO_H_
#define JPOV_GPU_INFO_H_

#include <cstdint>
#include <string>

namespace jpov {

// GPU 类别。**三态而非 bool**：真实世界有中间地带（WSL 的 D3D12 转译、virgl、
// ANGLE over WARP 都是「不是 llvmpipe 但也不完全是本机独显」），做成 bool 就是造假精度。
enum class GpuKind : uint8_t {
    // 查询失败：没有 active context，或 glGetString 返回了空（例如 context 刚销毁）。
    kUnknown = 0,

    // 未匹配任何已知软件光栅器 ⇒ **假定**是硬件。
    // 注意语义：这是「不是已知的软件光栅器」，不是「已确认是硬件 GPU」。
    kHardware = 1,

    // 明确匹配到软件光栅器：llvmpipe / softpipe / swrast / GDI Generic 等。
    // 这是唯一**可以确定**的判定（Mesa / 微软写的字符串稳定可匹配），
    // 所以「拒绝在软件光栅上跑」这类门禁一律以它为准（拒绝得准 > 放行得准）。
    kCpuRaster = 2,
};

// 类别名（ASCII，供 HUD / 日志用）：kUnknown → "unknown"、
// kHardware → "hardware"、kCpuRaster → "cpu-raster"。
const char* GpuKindName(GpuKind kind);

// 按 GL_RENDERER 字符串判定类别（纯函数，不碰 GL，可单测）。
//
// 规则（大小写不敏感的子串匹配）：
//   * 空串                                        → kUnknown
//   * 命中已知软件光栅器名（llvmpipe / softpipe / swrast /
//     software rasterizer / GDI Generic / Microsoft Basic Render）→ kCpuRaster
//   * 其余                                        → kHardware（语义见枚举注释：
//     “不是已知软件光栅器”，不是“已确认是硬件”）
//
// 为什么只对软件光栅器下断言：这些名字是 Mesa / 微软自己写的，稳定可匹配；
// 而“是真硬件”无法从字符串反推（WSL 的 D3D12 转译、virgl 都是中间地带）。
// 因此“拒绝在软件光栅上跑”这类门禁一律以 kCpuRaster 为准。
GpuKind ClassifyGpuRenderer(const std::string& renderer);

// 一块 GPU / 一套驱动的只读快照。
struct GpuInfo {
    // GL_RENDERER 原文。识别 llvmpipe / 具体型号的第一信息。
    // 空串 = 查询失败（见 kind == kUnknown）。
    std::string renderer;

    // GL_VENDOR 原文（"NVIDIA Corporation" / "Mesa/X.org" / "Intel" …）。
    std::string vendor;

    // GL_VERSION 原文（"4.6.0 NVIDIA 560.94" / "4.5 (Core Profile) Mesa 23.0.4" …）。
    std::string version;

    // GL_SHADING_LANGUAGE_VERSION 原文。
    std::string glsl_version;

    // 类别，见 GpuKind。默认 kUnknown（未查询）。
    GpuKind kind = GpuKind::kUnknown;

    // ---- GL 能力上限（0 = 未查到）----
    int max_texture_size        = 0;   // GL_MAX_TEXTURE_SIZE
    int max_vertex_attribs      = 0;   // GL_MAX_VERTEX_ATTRIBS
    int max_color_attachments   = 0;   // GL_MAX_COLOR_ATTACHMENTS（MRT 上限）
};

}  // namespace jpov

#endif  // JPOV_GPU_INFO_H_

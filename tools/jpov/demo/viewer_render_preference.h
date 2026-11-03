// JPOV 查看器层的渲染设备偏好 —— 交互式 viewer 声明「想要硬件光栅」的唯一入口
//
// 为什么需要这一层：设备偏好的三件事实（建 context 前的驱动提示 / 建 context 后的
// glGetString 核对 / 落空时的告警与排查清单）全在 interface/render_preference.h +
// jpov.cc 里。查看器这边只该表达一个事实 ——「我是交互式工具」—— 而不是每份
// main() 里各抄一遍四行注释。十几个 viewer 复制粘贴的注释，一定先于代码漂移。
//
// 为什么 headless 不设：CI / gold / --capture 本来就跑在 llvmpipe 上，
// 每次拍摄都刷一条「prefer-gpu 落空」告警是纯噪音；拍摄要的恰恰是**确定性**。
//
// 为什么「交互式」值得声明：Windows 混合显卡笔记本上，这个偏好配合 jpov.cc 里
// 恒导出的 NvOptimusEnablement / AmdPowerXpressRequestHighPerformance，能让驱动
// 优先挑独显；落到 llvmpipe / SwiftShader / WARP 上时，日志会明确写「为什么慢」，
// 而不是让人对着几 fps 猜。注意 JPOV **不能选设备**（见 gpu_info.h 顶部注释），
// 这只是偏好，最终仍由驱动 / 系统 per-app 设置决定。

#ifndef JPOV_DEMO_VIEWER_RENDER_PREFERENCE_H_
#define JPOV_DEMO_VIEWER_RENDER_PREFERENCE_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_viewer {

// 交互式查看器（有可见窗口、靠鼠标实时拖拽）的标准渲染设备偏好：prefer-gpu。
//
// 调用方**只在交互模式**调用（headless 保持默认 kAny）。
// 作用范围：只改 cfg.render_preference，不碰其他字段；必须在 Init() 之前调用
// （kPreferCpu 那条路会在建 context 前写环境变量，kPreferGpu 的核对发生在 Init 里）。
inline void PreferGpuForInteractiveViewer(JPOV::Config& cfg) {
    cfg.render_preference = jpov::RenderPreference::kPreferGpu;
}

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_VIEWER_RENDER_PREFERENCE_H_

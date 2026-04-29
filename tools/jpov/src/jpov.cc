// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环：
//   1. 创建 GLFW 窗口
//   2. 每帧：CaptureInput() → OneIteration() → RenderCommands()
//   3. 窗口关闭后清理退出

#include "tools/jpov/include/jpov/jpov.h"

#include <glog/logging.h>

JPOV::JPOV(Config cfg)
    : config_(cfg) {
}

JPOV::~JPOV() = default;

void JPOV::Run() {
    LOG(INFO) << "JPOV::Run() — starting window loop";
    LOG(INFO) << "  title:       " << config_.title;
    LOG(INFO) << "  size:        " << config_.width << "x" << config_.height;
    LOG(INFO) << "  resizable:   " << (config_.resizable ? "yes" : "no");
    LOG(INFO) << "  fullscreen:  " << (config_.fullscreen ? "yes" : "no");
    LOG(INFO) << "  show_console: " << (config_.show_console ? "yes" : "no");

    // TODO: 创建 GLFW 窗口，初始化 OpenGL 上下文

    int64_t frame = 0;

    while (true) {
        // TODO: 检测窗口关闭信号
        // if (glfwWindowShouldClose(...)) break;

        CaptureInput();

        // 构造输入结构（由 CaptureInput 填充输入）
        // TODO: 将 CaptureInput 的内容写入实际的 InputSnapshot
        jpov::InputSnapshot input{};

        // 构造窗口信息
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(config_.width);
        winfo.height = static_cast<float>(config_.height);

        // 渲染指令列表（用户在此填充）
        jpov::RenderCommandList cmds;

        OneIteration(frame, input, winfo, cmds);

        RenderCommands(cmds);

        // TODO: glfwSwapBuffers(...)
        // TODO: glfwPollEvents();

        ++frame;
    }

    LOG(INFO) << "JPOV::Run() — exiting";
}

void JPOV::CaptureInput() {
    // 暂未实现：采集 GLFW 事件队列，生成 InputSnapshot
}

void JPOV::RenderCommands(const jpov::RenderCommandList& cmds) {
    // 暂未实现：遍历 cmds，调用 OpenGL 绘制
    (void)cmds;
}

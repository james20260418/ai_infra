// JPOV Renderer — OpenGL 复杂度消化器
//
// Renderer 是 JPOV 的私有组件，管理所有 GL 资源的生命周期。
// 用户不直接访问 Renderer，所有操作通过 JPOV 的接口完成。
//
// 核心设计：FBO 固定 4096×4096，所有 2D 绘制在 FBO 空间中进行。
// Present 时从 FBO 左上角裁剪窗口大小区域，不做缩放。

#ifndef JPOV_RENDERER_H_
#define JPOV_RENDERER_H_

#include <cstdint>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/window_info.h"

struct GLFWwindow;

namespace jpov {

// Renderer — 离线 FBO + 指令消费 + GL 资源管理
//
// 初始化流程：
//   1. Renderer::Init() → 创建 FBO + shader + VBO
// 每帧：
//    a. BeginFrame()    → 绑定 FBO，清屏
//    b. Render()        → 消费 RenderCommandList
//    c. Present()       → FBO 窗口区域 → 默认 framebuffer（无缩放）
//
// 坐标约定：
//   - 2D 坐标以 FBO 空间为单位（4096×4096），非窗口像素
//   - shader 用 fbo_width_/fbo_height_ 做 NDC 变换
//   - Present 只取窗口大小区域，不缩放
struct Renderer {
    static constexpr int kFboWidth  = 4096;
    static constexpr int kFboHeight = 4096;
    static constexpr int kMaxStreamVertices = 1000;

    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 初始化所有 GL 资源
    // Pre-condition: GL context 已创建且当前
    void Init();

    // 开始一帧：绑定 FBO，清屏
    void BeginFrame();

    // 消费渲染指令列表，绘制到 FBO
    void Render(const RenderCommandList& cmds, const Camera& camera,
                const WindowInfo& winfo);

    // 将 FBO 中窗口大小区域 blit 到默认 framebuffer（无缩放）
    // src: (0, 0, window_width, window_height) → dst: (0, 0, fb_w, fb_h)
    void Present(GLFWwindow* window, int window_width, int window_height);

private:
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;
    unsigned int stream_vbo_ = 0;
    unsigned int prog_ = 0;

    void CreateFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void DrawRect2D(const Rect2DCommand& cmd, const WindowInfo& winfo);
    void DrawPolyline2D(const Polyline2DCommand& cmd, const WindowInfo& winfo);
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

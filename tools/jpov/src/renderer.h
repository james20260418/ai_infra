// JPOV Renderer — OpenGL 复杂度消化器
//
// 离线 FBO + 指令消费。FBO 尺寸根据 RenderCommandList::render_resolution
// 动态调整，分辨率不变时不重建。
//
// 所有 2D 坐标以渲染分辨率为空间（非窗口坐标），
// Present 时从 FBO 裁剪窗口大小区域 → framebuffer（无缩放）。

#ifndef JPOV_RENDERER_H_
#define JPOV_RENDERER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/window_info.h"

struct GLFWwindow;

namespace jpov {

struct Renderer {
    static constexpr int kMaxFboDim = 4096;
    static constexpr int kMaxStreamVertices = 1000;

    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Init: 初始化 shader + VBO。Pre-condition: GL context 当前
    void Init();

    // BeginFrame: 绑定 FBO。如果 render_resolution 与当前 FBO 不一致则重建
    void BeginFrame(int render_w, int render_h);

    // Render: 消费指令列表绘制到 FBO
    void Render(const RenderCommandList& cmds, const Camera& camera,
                const WindowInfo& winfo);

    // Present: FBO[0,0,win_w,win_h] → framebuffer（无缩放，GL_LINEAR）
    void Present(GLFWwindow* window, int window_width, int window_height);

    // SaveScreenshot: 截取窗口尺寸画面，保存为 PNG 文件
    //
    // 内部维护一个窗口尺寸的 output FBO，将渲染 FBO 拉伸到 output FBO 后读取，
    // 模拟 Present 到窗口后截图的视觉效果。
    //
    // Pre-condition: win_w > 0 && win_h > 0
    // Pre-condition: 已调用过 BeginFrame 且渲染 FBO 已初始化
    void SaveScreenshot(int win_w, int win_h, const char* path);

    // SaveScreenshotToBuffer: 同上，但以 RGBA uint8 数组输出
    void SaveScreenshotToBuffer(int win_w, int win_h,
                                std::vector<uint8_t>* out_pixels /*output*/);

private:
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;
    unsigned int stream_vbo_ = 0;
    unsigned int prog_ = 0;
    int fbo_w_ = 0;
    int fbo_h_ = 0;

    // Output FBO：窗口尺寸，用于截图时拉伸渲染 FBO 并读取像素
    unsigned int out_fbo_ = 0;
    unsigned int out_color_tex_ = 0;
    int out_w_ = 0;
    int out_h_ = 0;

    void EnsureFBO(int width, int height);
    void EnsureOutputFBO(int win_w, int win_h);
    void DestroyFBO();
    void DestroyOutputFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void DrawRect2D(const Rect2DCommand& cmd);
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

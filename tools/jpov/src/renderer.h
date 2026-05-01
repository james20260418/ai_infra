// JPOV Renderer — OpenGL 复杂度消化器
//
// Renderer 是 JPOV 的私有组件，管理所有 GL 资源的生命周期。
// 用户不直接访问 Renderer，所有操作通过 JPOV 的接口完成。
//
// 注意：此头文件不使用任何 GL 类型（GLuint, GLenum 等），
// 以避免在未 include GL 头文件的上下文中编译错误。
// .cc 文件中再 include 完整的 GL header。

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
//   每帧：
//     a. BeginFrame()    → 绑定 FBO，清屏
//     b. Render()         → 消费 RenderCommandList
//     c. Present()        → FBO → 默认 framebuffer blit
// 截图：
//   CapturePixels() → glReadPixels 返回 RGBA buffer
struct Renderer {
    static constexpr int kMaxFboWidth  = 4096;
    static constexpr int kMaxFboHeight = 4096;
    static constexpr int kMaxStreamVertices = 10000;

    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 初始化所有 GL 资源。Pre-condition: GL context 已创建且当前
    void Init(int window_width, int window_height);

    // 开始一帧：绑定 FBO，设置视口为窗口尺寸，清除颜色缓冲
    void BeginFrame(int window_width, int window_height);

    // 消费渲染指令列表，绘制到当前 FBO
    // 2D 坐标以窗口像素为单位（winfo.width/height），shader 内部转换为 NDC
    void Render(const RenderCommandList& cmds, const Camera& camera,
                const WindowInfo& winfo);

    // 将 FBO 中窗口大小区域 blit 到默认 framebuffer（窗口显示）
    // 不做缩放——FBO 尺寸 ≥ 窗口尺寸时，只取窗口大小的左上角区域
    void Present(GLFWwindow* window, int window_width, int window_height);

    // 从 FBO 读取像素到 CPU 内存
    // out_pixels: 大小为 width * height * 4 (RGBA, uint8_t)
    void CapturePixels(uint8_t* out_pixels /*output*/, int width, int height);

    int FboWidth() const { return fbo_width_; }
    int FboHeight() const { return fbo_height_; }

private:
    // 不暴露 GL 类型，用 unsigned int 代替 GLuint
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;
    unsigned int stream_vbo_ = 0;
    unsigned int polyline2d_prog_ = 0;
    int fbo_width_ = 0;
    int fbo_height_ = 0;

    void CreateFBO(int width, int height);
    void CompileShaders();
    void CreateStreamVBO();
    void DrawPolyline2D(const Polyline2DCommand& cmd, const WindowInfo& winfo);
    void DrawRect2D(const Rect2DCommand& cmd, const WindowInfo& winfo);
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

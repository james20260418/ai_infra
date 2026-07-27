// JPOV Renderer — OpenGL 复杂度消化器
//
// 离线 FBO + 指令消费。FBO 尺寸根据 RenderCommandList::render_resolution
// 动态调整，分辨率不变时不重建。
//
// 所有 2D 坐标以渲染分辨率为空间（非窗口坐标），
// Present 时从 FBO 裁剪窗口大小区域 → framebuffer（无缩放）。

#ifndef JPOV_RENDERER_H_
#define JPOV_RENDERER_H_

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/font_manager.h"

struct GLFWwindow;

namespace jpov {

struct Renderer {
    static constexpr int kMaxFboDim = 4096;
    static constexpr int kMaxPolylineEdges = 10000;
    static constexpr int kMaxStreamVertices = 120000;

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
    void DrawPolyline2D(const Polyline2DCommand& cmd);
    void DrawCircle2D(const Circle2DCommand& cmd);
    void DrawText2D(const Text2DCommand& cmd);

    // ---- 字体管理 ----

    // 每种字体资源：FontManager + 三层 GL atlas 纹理（16/32/48px）。
    // FontManager 不持有 GL 资源，图集纹理由 Renderer 创建和管理。
    //
    // 目前两种字体：CJK（中日韩，TTC font_index=0）和 Latin fallback（DejaVuSans）。
    struct FontSlot {
        std::optional<FontManager> manager;
        unsigned int atlas_tex[3] = {0, 0, 0};  // [0]=16px, [1]=32px, [2]=48px
    };

    // 构造两个 FontSlot（CJK + Latin fallback），创建三层 GL atlas 纹理
    void InitFonts();

    // 上传指定层级 atlas 到 GL（仅在 atlas_dirty 时）
    void UploadAtlas(FontSlot& slot, int level);

    // 上传所有脏层级
    void UploadAllDirty(FontSlot& slot);

    FontSlot font_cjk_;
    FontSlot font_latin_;
    unsigned int tex_prog_ = 0;
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

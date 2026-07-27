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

struct GLFWwindow;
struct stbtt_fontinfo;

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

    void LoadFont();
    void DestroyFont();
    void UploadAtlas();

    struct GlyphBitmap {
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        float advance = 0.0f;
        float xoff = 0.0f;
        float yoff = 0.0f;
        int atlas_x = 0;  // packed x position in atlas (pixel)
        int atlas_y = 0;  // packed y position in atlas (pixel)
    };
    std::unordered_map<uint32_t, GlyphBitmap> font_glyphs_;  // codepoint → glyph
    stbtt_fontinfo* font_info_ = nullptr;
    unsigned char* font_ttf_data_ = nullptr;
    unsigned int font_atlas_tex_ = 0;
    int font_atlas_w_ = 0;
    int font_atlas_h_ = 0;
    unsigned int tex_prog_ = 0;
    bool font_loaded_ = false;
    float font_ascent_ = 0.0f;
    float font_descent_ = 0.0f;
    float font_linegap_ = 0.0f;

    // ---- UTF-8 编码常量 ----
    static constexpr uint8_t kUTF8ContByte = 0x80;    // 10xxxxxx - continuation byte marker
    static constexpr uint8_t kUTF8ContMask = 0x3F;    // continuation data: 6 low bits
    static constexpr uint8_t kUTF8Lead2Byte = 0xC0;   // 110xxxxx - 2-byte start
    static constexpr uint8_t kUTF8Lead2Mask = 0x1F;   // 2-byte data: 5 low bits
    static constexpr uint8_t kUTF8Lead3Byte = 0xE0;   // 1110xxxx - 3-byte start
    static constexpr uint8_t kUTF8Lead3Mask = 0x0F;   // 3-byte data: 4 low bits
    static constexpr uint8_t kUTF8Lead4Byte = 0xF0;   // 11110xxx - 4-byte start
    static constexpr uint8_t kUTF8Lead4Mask = 0x07;   // 4-byte data: 3 low bits
    static constexpr uint32_t kUTF8Replacement = 0xFFFD;  // U+FFFD replacement character

    // Dynamic glyph atlas: 4096x4096, row-by-row packing
    static constexpr int kAtlasSize = 4096;
    static constexpr int kGlyphPadding = 2;  // pixels between glyphs (avoids bleeding)
    static constexpr float kBaseFontSize = 16.0f;
    static constexpr int kAtlasUploadLogInterval = 5;  // LOG_EVERY_N interval for atlas uploads
    static constexpr int kFontNotLoadedLogInterval = 60;  // LOG_EVERY_N interval for missing font warnings

    // Host-side atlas bitmap (grayscale)
    std::vector<unsigned char> atlas_pixels_;
    int atlas_cursor_x_ = 0;   // next free x position in current row
    int atlas_cursor_y_ = 0;   // current row start y
    int atlas_row_h_ = 0;      // height of current row
    bool atlas_dirty_ = false; // true if glyphs were added after last GL upload

    // Get or rasterize a glyph for the given Unicode codepoint.
    // Returns pointer to glyph entry (inserted if new).
    GlyphBitmap* GetOrRasterizeGlyph(uint32_t cp);

    // UTF-8 decode helper: returns codepoint and advances pointer
    static uint32_t UTF8Decode(const char*& p);
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

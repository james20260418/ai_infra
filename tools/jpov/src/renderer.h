// JPOV Renderer — OpenGL 复杂度消化器
#ifndef JPOV_RENDERER_H_
#define JPOV_RENDERER_H_

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/font2d/font_renderer.h"
#include "tools/jpov/src/mesh_manager.h"
#include "tools/jpov/src/object3d/object3d_renderer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"

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

    void Init(const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
              const std::vector<std::tuple<const char*, int, const char*>>& default_fonts);
    void BeginFrame(int render_w, int render_h);
    void Render(const RenderCommandList& cmds, const WindowInfo& winfo);
    void Present(GLFWwindow* window, int window_width, int window_height);
    void SaveScreenshot(int win_w, int win_h, const char* path);
    void SaveScreenshotToBuffer(int win_w, int win_h, std::vector<uint8_t>* out_pixels);

    TextureManager& GetTextureManager() { return texture_mgr_; }
    MeshManager& GetMeshManager() { return mesh_mgr_; }

private:
    unsigned int fbo_ = 0, color_tex_ = 0, stream_vbo_ = 0;
    int fbo_w_ = 0, fbo_h_ = 0;

    unsigned int tile_index_tex_ = 0;
    int tile_grid_w_ = 0, tile_grid_h_ = 0, tile_tex_w_ = 0, tile_tex_h_ = 0;

    unsigned int out_fbo_ = 0, out_color_tex_ = 0;
    int out_w_ = 0, out_h_ = 0;

    unsigned int fbo_3d_ = 0, color_tex_3d_ = 0, depth_rb_3d_ = 0, depth_tex_3d_ = 0;
    int fbo_3d_w_ = 0, fbo_3d_h_ = 0;

    unsigned int resolve_fbo_3d_ = 0, resolve_tex_3d_ = 0;
    int resolve_fbo_3d_w_ = 0, resolve_fbo_3d_h_ = 0;

    void EnsureFBO(int w, int h);
    void EnsureOutputFBO(int w, int h);
    void Ensure3DFBO(int w, int h);
    void DestroyFBO();
    void DestroyOutputFBO();
    void Destroy3DFBO();
    void Destroy3DResolveFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void DrawRect2D(const Rect2DCommand& cmd);
    void DrawPolyline2D(const Polyline2DCommand& cmd);
    void DrawCircle2D(const Circle2DCommand& cmd);
    void DrawTriangle3D(const Triangle3DCommand& cmd);
    void DrawLine3D(const Line3DCommand& cmd);
    void DrawText3D(const Text3DCommand& cmd);
    void Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h);

    unsigned int strip_vbo_ = 0;
    static constexpr int kMaxStripVertices = 3000;
    void DrawStrip3D(const Strip3DCommand& cmd);

    static constexpr int kRoundCornerSegments = 12;
    static constexpr int kCircleFanSegments = 64;
    static constexpr int kArcFullCircleSegments = 48;
    static constexpr int kMaxStrip2DVertices = 3000;

    void DrawStrip2D(const Strip2DCommand& cmd);
    void DrawRoundRect2D(const RoundRect2DCommand& cmd);
    void DrawFillRect2D(const FillRect2DCommand& cmd);
    void DrawArc2D(const Arc2DCommand& cmd);
    void DrawImage2D(const Image2DCommand& cmd);

    static std::vector<float> TriangulateRoundRectFill(const Vec2f& pos, const Vec2f& size, float radius);

    float mvp_[16];
    ShaderManager shader_mgr_;

    unsigned int SolidProg();
    unsigned int TextProg();
    unsigned int ImageProg();
    unsigned int Solid3DProg();
    unsigned int Text3DProg();
    unsigned int DrawObject3DProg();
    unsigned int DrawObject3DProgFull();

    TextureManager texture_mgr_;
    FontRenderer font_renderer_;
    MeshManager mesh_mgr_;
};

}  // namespace jpov

#endif

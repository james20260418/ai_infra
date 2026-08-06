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
#include <tuple>
#include <unordered_map>
#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/font_manager.h"
#include "tools/jpov/src/mesh_manager.h"
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

    // Init: 初始化 shader + VBO + 字体。Pre-condition: GL context 当前
    // font_entries: 用户配置的字体列表（(path, ttc_index, alias)）
    // default_fonts:  内置默认字体列表（(path, ttc_index, alias)）
    // 用户与内置共享别名空间，初始化时 alias 查重
    void Init(const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
              const std::vector<std::tuple<const char*, int, const char*>>& default_fonts);

    // BeginFrame: 绑定 FBO。如果 render_resolution 与当前 FBO 不一致则重建
    void BeginFrame(int render_w, int render_h);

    // Render: 消费指令列表绘制到 FBO
    // Camera 从 cmds.camera 读取
    void Render(const RenderCommandList& cmds, const WindowInfo& winfo);

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

    // 纹理管理器（暴露给 JPOV 用于 RegisterTexture 等）
    TextureManager& GetTextureManager() { return texture_mgr_; }

    // 网格管理器（暴露给 JPOV 用于 RegisterMesh / UpdateMesh / ReleaseMesh）
    MeshManager& GetMeshManager() { return mesh_mgr_; }

private:
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;
    unsigned int stream_vbo_ = 0;
    int fbo_w_ = 0;
    int fbo_h_ = 0;

    // ---- Tiled Forward 光照：tile 光源 index 纹理 ----
    // 每帧 CPU 端做 culling，把影响每个 tile（16×16 像素）的 ≤16 个光源 index
    // 写入 tile_index_tex，供 fragment shader 按 gl_FragCoord 查表。
    // tile 网格与 3D FBO 同尺寸，随 Ensure3DFBO 一起重建。
    //
    // 下面的常量与 renderer.cc 的 kMeshFs3dTiledLighting GLSL #define 一一对应：
    //   kTileSize16_       ↔ TILE_SIZE
    //   kMaxLightsPerTile_ ↔ MAX_LIGHTS_PER_TILE
    //   kMaxTotalLights_   ↔ MAX_TOTAL_LIGHTS
    //   kLightIndexSentinel_ ↔ LIGHT_INDEX_SENTINEL
    // 改任一处必须同时改另一处，否则 tile 编码/解码错位。
    static constexpr int kTileSize16_ = 16;
    static constexpr int kMaxLightsPerTile_ = 16;
    // 每 tile 用 4 个 texel，每 texel RGBA 各存 1 个 uint8 index（4×4=16）
    static constexpr int kTexelsPerTile_ = kMaxLightsPerTile_ / 4;
    // 光源 index 用 uint8 编码，最多 255 个光源。
    // 哨兵 255 = 无光源（与 shader 的 LIGHT_INDEX_SENTINEL 一致），有效 index 0-254。
    static constexpr int kMaxTotalLights_ = 255;
    static constexpr uint8_t kLightIndexSentinel_ = 255;

    unsigned int tile_index_tex_ = 0;  // GL_RGBA8, 宽 = grid_cols*4, 高 = grid_rows
    int tile_grid_w_ = 0;              // tile 网格列数
    int tile_grid_h_ = 0;              // tile 网格行数

    // 销毁 tile 纹理（在 Destroy3DFBO 配合调用）
    void DestroyTileLighting();
    // 分配/重建 tile 纹理（在 Ensure3DFBO 里调用）
    void EnsureTileLighting(int fbo_w_3d, int fbo_h_3d);
    // CPU 端 culling：遍历 point_lights，把影响每个 tile 的 ≤16 个光源 index
    // 写入 tile_index_tex_。在绘制任何 3D Object 前调用。
    void BuildTileLightIndices(const RenderCommandList& cmds, int fbo_w_3d, int fbo_h_3d);
    // 上传全部光源数据到 lighting shader 的 flat uniform 数组
    //（在 BuildTileLightIndices 之后调用，用 MeshLighting3DProg() 作目标 program）
    void UploadLightData(const RenderCommandList& cmds);

    // Output FBO：窗口尺寸，用于截图时拉伸渲染 FBO 并读取像素
    unsigned int out_fbo_ = 0;
    unsigned int out_color_tex_ = 0;
    int out_w_ = 0;
    int out_h_ = 0;

    // 3D 离屏 MSAA FBO：用于 3D 命令的渲染
    // 使用 GL_RGBA8 + 4x MSAA，按 Camera 的三维分辨率分配
    // 渲染完成后通过 glBlitFramebuffer blit 到主 FBO
    unsigned int fbo_3d_ = 0;
    unsigned int color_tex_3d_ = 0;
    unsigned int depth_rb_3d_ = 0;     // 深度 renderbuffer（MSAA 路径）
    unsigned int depth_tex_3d_ = 0;    // 深度纹理（非 MSAA 路径）
    int fbo_3d_w_ = 0;
    int fbo_3d_h_ = 0;

    // 中间 non-MSAA resolve FBO：同 MSAA FBO 尺寸，
    // 用于 MSAA resolve 后再缩放 blit 到主 FBO
    unsigned int resolve_fbo_3d_ = 0;
    unsigned int resolve_tex_3d_ = 0;
    int resolve_fbo_3d_w_ = 0;
    int resolve_fbo_3d_h_ = 0;

    void EnsureFBO(int width, int height);
    void EnsureOutputFBO(int win_w, int win_h);
    void Ensure3DFBO(int width, int height);
    void DestroyFBO();
    void DestroyOutputFBO();
    void Destroy3DFBO();
    void Destroy3DResolveFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void DrawRect2D(const Rect2DCommand& cmd);
    void DrawPolyline2D(const Polyline2DCommand& cmd);
    void DrawCircle2D(const Circle2DCommand& cmd);
    void DrawText2D(const Text2DCommand& cmd);
    // 3D 方法：MVP 矩阵通过成员 mvp_ 传递（由 Render/Draw3DCommands 设置）
    void DrawTriangle3D(const Triangle3DCommand& cmd);
    void DrawLine3D(const Line3DCommand& cmd);
    void DrawText3D(const Text3DCommand& cmd);
    void DrawObject3D(const Object3DCommand& cmd, const RenderCommandList& cmds);
    void Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h);

    // ---- 字体管理 ----

    // 每种字体资源：FontManager + 三层 GL atlas 纹理（16/32/48px）。
    // FontManager 不持有 GL 资源，图集纹理由 Renderer 创建和管理。
    struct FontSlot {
        std::optional<FontManager> manager;
        unsigned int atlas_tex[3] = {0, 0, 0};  // [0]=16px, [1]=32px, [2]=48px
    };

    // 初始化所有字体（用户 + 内置）。
    // 每个条目：(path, ttc_index, alias)。
    // 用户与内置共享别名空间，失败或别名重复 → LOG(FATAL) crash。
    void InitFonts(const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
                   const std::vector<std::tuple<const char*, int, const char*>>& default_fonts);

    // 上传指定层级 atlas 到 GL（仅在 atlas_dirty 时）
    void UploadAtlas(FontSlot& slot, int level);

    // 上传所有脏层级
    void UploadAllDirty(FontSlot& slot);

    // 按别名查找字体 slot（空别名或未命中 → 返回第一个）
    FontSlot* FindFontSlot(const std::string& alias);

    // 初始化单个 FontSlot（静态方法，供 InitFonts 调用）
    static void InitOneFontSlot(const char* alias,
                                 const std::string& resolved_path,
                                 int ttc_index,
                                 FontSlot* slot);

    // 注册一个字体到 font_slots_（含查重/路径检测等逻辑）
    static void RegisterFont(const char* path,
                              int ttc_index,
                              const char* alias,
                              const char* source,
                              std::unordered_map<std::string, FontSlot>* font_slots,
                              std::vector<std::string>* font_order);

    // alias → FontSlot 映射
    std::unordered_map<std::string, FontSlot> font_slots_;
    // 注册顺序（用于空 alias 回退到第一个）
    std::vector<std::string> font_order_;

    // Strip3D 专用 VBO（3000 顶点缓存，初始化时分配）
    unsigned int strip_vbo_ = 0;
    static constexpr int kMaxStripVertices = 3000;

    void DrawStrip3D(const Strip3DCommand& cmd);

    // ---- 2D 图元常量 ----
    // 圆角三角化分段数（每个 90° 圆角细分成 kRoundCornerSegments 个扇形三角形）
    static constexpr int kRoundCornerSegments = 12;
    // 完整圆的扇形三角形分段数
    static constexpr int kCircleFanSegments = 64;
    // 圆弧近似分段数（用于 DrawCircle2D 和 DrawArc2D 的完整圆路径）
    static constexpr int kArcFullCircleSegments = 48;

    // 2D 条带（屏幕空间，像素坐标，GL_TRIANGLE_STRIP）
    static constexpr int kMaxStrip2DVertices = 3000;
    void DrawStrip2D(const Strip2DCommand& cmd);
    void DrawRoundRect2D(const RoundRect2DCommand& cmd);
    void DrawFillRect2D(const FillRect2DCommand& cmd);
    void DrawArc2D(const Arc2DCommand& cmd);

    // ---- 圆角矩形填充三角化（共享方法） ----
    // 将圆角矩形（pos, size, radius）三角化为 GL_TRIANGLES 顶点列表。
    // 返回：每个顶点 2 个 float (x, y)，按 9 区域拓扑排列。
    // radius=0 时退化为普通矩形（4 顶点 GL_TRIANGLE_FAN）。
    // Pre-condition: size.x > 0 && size.y > 0
    // Pre-condition: radius >= 0
    static std::vector<float> TriangulateRoundRectFill(
        const Vec2f& pos, const Vec2f& size, float radius);

    float mvp_[16];  // 当前帧的 MVP 矩阵缓存

    // Shader 管理器（所有 GLSL program 的注册 / 编译 / 缓存）
    ShaderManager shader_mgr_;

    // ---- Shader program 访问器 ----
    // 通过 ShaderManager 按名字获取，首次调用时用对应 GLSL 源码注册。
    // GetOrCreate 幂等：重复调用返回缓存的 program。
    // 新增 shader 时在此加一个访问器即可，无需改 Renderer 成员变量。
    unsigned int SolidProg();     // 2D 纯色（kVs/kFs）
    unsigned int TextProg();      // 2D 文字/字体（kTexVs/kTexFs）
    unsigned int ImageProg();     // 2D 图片 RGBA（kTexVs/kImageFs）
    unsigned int Solid3DProg();   // 3D 纯色（kVs3d/kFs3d）
    unsigned int Text3DProg();    // 3D 纹理（kTexVs3d/kTexFs，为 Text3D 预留）
    unsigned int TexturedMesh3DProg();  // 3D 静态模型纹理（kMeshVs3d/kMeshTexFs）
    unsigned int MeshLighting3DProg();  // 3D 静态模型光照（kMeshVs3dLighting/kMeshFs3dLighting）

    // 纹理管理器（字体 atlas 之外的用户纹理）
    TextureManager texture_mgr_;

    // 网格管理器（CPU MeshData → GPU VAO/VBO/EBO 的上传/更新/释放）
    MeshManager mesh_mgr_;

    // 2D 图片渲染
    void DrawImage2D(const Image2DCommand& cmd);
};

}  // namespace jpov

#endif  // JPOV_RENDERER_H_

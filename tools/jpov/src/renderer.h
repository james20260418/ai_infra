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
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/font2d/font_renderer.h"
#include "tools/jpov/src/mesh_manager.h"
#include "tools/jpov/src/object3d/object3d_renderer.h"
#include "tools/jpov/src/primitives2d/primitives2d_renderer.h"
#include "tools/jpov/src/primitives3d/primitives3d_renderer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/skydome/sky_renderer.h"
#include "tools/jpov/src/texture_manager.h"

struct GLFWwindow;

namespace jpov {

struct Renderer {
    static constexpr int kMaxFboDim = 4096;
    static constexpr int kMaxStreamVertices = 120000;

    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Init(const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
              const std::vector<std::tuple<const char*, int, const char*>>& default_fonts,
              const ShadowConfig& shadow_cfg);
    void BeginFrame(int render_w, int render_h);
    void Render(const RenderCommandList& cmds, const WindowInfo& winfo);
    void Present(GLFWwindow* window, int window_width, int window_height);
    void SaveScreenshot(int win_w, int win_h, const char* path);
    void SaveScreenshotToBuffer(int win_w, int win_h, std::vector<uint8_t>* out_pixels);

    TextureManager& GetTextureManager() { return texture_mgr_; }
    MeshManager& GetMeshManager() { return mesh_mgr_; }

    // ========== glTF 模型加载（用户可见入口） ==========
    //
    // LoadGltf: 从 .gltf/.glb 文件加载整个模型并上传 GPU 资源。
    //   - 内部调用纯净 loader（tinygltf → MeshData/材质路径，无 GL）
    //   - 用 MeshManager 上传几何、TextureManager 上传贴图
    //   - 多 mesh / 共享贴图自动去重
    //   - ORM (metallicRoughnessTexture) 自动拆分为 AO/Roughness/Metallic
    //   - 返回 GltfObject（资源独占，见 gltf_object.h）
    //
    // 返回的 GltfObject 用 RenderCommandList::DrawGltfObject() 渲染，
    // 用 ReleaseGltf() 释放。
    //
    // Pre-condition: GL context 已激活（Init 之后）
    // 失败: 返回空 GltfObject（empty()）。
    GltfObject LoadGltf(const std::string& path);

    // ReleaseGltf: 释放一个 GltfObject 占用的全部 GPU 资源。
    //
    // 仅释放本 gltf 加载时创建的资源（独享约定，见 gltf_object.h）。
    // 同一 gltf 重复调用安全（Manager Release 幂等）。
    // Pre-condition: GL context 已激活（或正在析构）
    void ReleaseGltf(const GltfObject& gltf);

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

    // 太阳正交阴影 pass 的级联 FBO + 深度贴图（RenderCommandList.sun 有值时创建）。
    // 每一级联独立 FBO/深度纹理/尺寸，数量 = ShadowConfig::cascade_count。
    // tex 实为 RGBA32F 颜色纹理（存光空间 ndc.z），rb 为配套 depth renderbuffer
    //（仅遮挡测试用，见 EnsureShadowFBO）。
    std::vector<CascadeFBO> shadow_fbos_;   // 长度 = cascade_count（每帧按配置重建）
    float shadow_vp_[ShadowConfig::kMaxCascades][16]; // 各级联光空间 ViewProj
    ShadowConfig shadow_cfg_;               // 当前生效的阴影配置（Init 传入）

    void EnsureFBO(int w, int h);
    void EnsureOutputFBO(int w, int h);
    void Ensure3DFBO(int w, int h);
    void EnsureShadowFBO(const ShadowConfig& cfg);
    void DestroyFBO();
    void DestroyOutputFBO();
    void Destroy3DFBO();
    void Destroy3DResolveFBO();
    void DestroyShadowFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h);
    void DrawShadowPass(const RenderCommandList& cmds);

    unsigned int strip_vbo_ = 0;

    float mvp_[16];
    ShaderManager shader_mgr_;

    unsigned int SolidProg();
    unsigned int TextProg();
    unsigned int ImageProg();
    unsigned int Solid3DProg();
    unsigned int Text3DProg();
    unsigned int DrawObject3DProg();
    unsigned int DrawObject3DProgFull();
    unsigned int ShadowProg();

    TextureManager texture_mgr_;
    FontRenderer font_renderer_;
    MeshManager mesh_mgr_;
};

}  // namespace jpov

#endif

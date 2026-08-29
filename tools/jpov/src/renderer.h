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

    // 上一次拾取查询的结果（方法甲：渲染时填入 last_pick_，下帧读取）。
    const PickResult& last_pick() const { return last_pick_; }

    // 测量文本以指定字号的绘制宽度（像素），语义与 DrawText2D 的布局推进
    // 完全一致（pen 水平终点 = 光标 X）。alias 空串 → 首个注册字体；
    // 未知别名 → crash（与 DrawText2D 的字体查找一致）。
    // 供交互层（如 UI 输入框光标）用真实字体进宽定位，避免单一字符常数
    // 对混合 Latin/CJK 文本失效（Latin≈0.4em / CJK≈0.69em，0.6em 偏宽）。
    // Pre-condition: font_size > 0
    float MeasureTextWidth(const std::string& alias, const std::string& text,
                           float font_size);

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
    // Strip3D 专用 VBO（见 CreateStreamVBO）。
    unsigned int strip_vbo_ = 0;
    // 流式绘制 VAO（GL_POINTS 高亮回填等）：core profile 下顶点属性需绑定 VAO
    // 才合法。与 stream_vbo_ 一起 CreateStreamVBO 生成、Finalize 销毁。
    unsigned int stream_vao_ = 0;
    int fbo_w_ = 0, fbo_h_ = 0;

    unsigned int tile_index_tex_ = 0;
    int tile_grid_w_ = 0, tile_grid_h_ = 0, tile_tex_w_ = 0, tile_tex_h_ = 0;

    unsigned int out_fbo_ = 0, out_color_tex_ = 0;
    int out_w_ = 0, out_h_ = 0;

    unsigned int fbo_3d_ = 0, color_tex_3d_ = 0, depth_rb_3d_ = 0, depth_tex_3d_ = 0;
    int fbo_3d_w_ = 0, fbo_3d_h_ = 0;

    unsigned int resolve_fbo_3d_ = 0, resolve_tex_3d_ = 0;
    int resolve_fbo_3d_w_ = 0, resolve_fbo_3d_h_ = 0;

    // HDR 3D FBO（RGBA16F 颜色 + depth）：3D 内容（天空 + object3d + primitives3d）
    // 统一渲染目标，可存 >1.0 的 HDR 亮度。渲染完成后由统一后处理 pass
    // （tone map）压缩到 LDR。MSAA 路径下保留 separate resolve FBO（16F）。
    unsigned int fbo_hdr_ = 0, color_tex_hdr_ = 0, depth_rb_hdr_ = 0, depth_tex_hdr_ = 0;
    int fbo_hdr_w_ = 0, fbo_hdr_h_ = 0;
    unsigned int resolve_fbo_hdr_ = 0, resolve_tex_hdr_ = 0;
    int resolve_fbo_hdr_w_ = 0, resolve_fbo_hdr_h_ = 0;

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
    void EnsureHDRFBO(int w, int h);
    void EnsureShadowFBO(const ShadowConfig& cfg);
    void DestroyFBO();
    void DestroyOutputFBO();
    void Destroy3DFBO();
    void Destroy3DResolveFBO();
    void DestroyHDRFBO();
    void DestroyHDRResolveFBO();
    void DestroyShadowFBO();
    void CompileShaders();
    void CreateStreamVBO();
    void Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h);
    void DrawShadowPass(const RenderCommandList& cmds, const DirectionalLight& sun);

    // 拾取：color-ID pass。cmds.pick.enabled 时，把 picking_id>0 的物体用
    // 纯色 ID shader 画进离屏 pick FBO，glReadPixels 解码光标像素 → last_pick_。
    // fbo_w/fbo_h 为 3D FBO 尺寸；vp_x/y/w/h 为当前生效的 viewport（窗口坐标）。
    void DrawPickingPass(const RenderCommandList& cmds, int fbo_w, int fbo_h,
                         float vp_x, float vp_y, float vp_w, float vp_h);

    // 高亮 pass（CPU 屏幕空间回填）：3D 内容全部画完后统一执行的一个独立子步骤
    // （与 shadow pass / tone map pass 并列）。
    // 从 fbo_hdr_（MSAA 时自动 resolve）blit color 到 color-only 单采样 hl FBO，
    // 再把被高亮物体画进独立 mask 纹理（不扩张的单色剪影），CPU 读回 mask、
    // 做 outline_px 次像素膨胀，膨胀图与原剪影相减得恒定像素宽的边缘环，
    // 最后用 GL_POINTS 把边框色回填叠加到场景颜色上（hl_color_tex_）。
    // 返回叠加了高亮的颜色纹理（hl_color_tex_），供 tone map pass 作为输入。
    // 调用前提：use_hdr=true（HDR 3D FBO 已存在），且 cmds.highlight_style 有值。
    unsigned int DrawHighlightPass(const RenderCommandList& cmds,
                                   int fbo_w, int fbo_h);
    void EnsureHighlightFBO(int w, int h);
    void DestroyHighlightFBO();
    void EnsureHighlightMaskFBO(int w, int h);
    void DestroyHighlightMaskFBO();

    // Bloom（辉光）pass：HDR 高亮提取→多级半分辨率降采样模糊→上采样叠加
    // →加回 HDR 原图。确定性后处理，在 tone map **之前**工作。
    // 输入 hdr_input_tex（单采样浮点 HDR 纹理），返回加回辉光后的 HDR 纹理
    //（同输入尺寸的 bloom_work_tex_，供 tone map pass 采样）。
    // 调用前提：use_hdr=true，且 cmds.bloom 有值且 enabled。
    // 无 bloom 时返回原 hdr_input_tex（零开销零回归）。
    unsigned int DrawBloomPass(const RenderCommandList& cmds,
                               const BloomConfig& cfg,
                               unsigned int hdr_input_tex,
                               int hdr_w, int hdr_h);
    void EnsureBloomChain(int full_w, int full_h, int levels);
    void DestroyBloomChain();

    float mvp_[16];
    ShaderManager shader_mgr_;

    // 拾取查询结果（上一次 DrawPickingPass 的 outcome）。
    // 每帧在 DrawPickingPass 中覆盖；未发起查询时保持 hit=false。
    PickResult last_pick_;

    // 拾取离屏 FBO（RGBA8 颜色 + depth renderbuffer），尺寸 = 3D FBO。
    unsigned int pick_fbo_ = 0, pick_tex_ = 0, pick_depth_rb_ = 0;
    int pick_fbo_w_ = 0, pick_fbo_h_ = 0;

    // 高亮叠加 FBO：color-only 单采样（RGBA16F）。blit 场景 color 到此处后，
    // 在此叠加恒定像素宽边框（CPU 剪影膨胀求边缘环）。
    // 完成后其颜色纹理作为 tone map 的输入。
    unsigned int hl_fbo_ = 0, hl_color_tex_ = 0;
    int hl_fbo_w_ = 0, hl_fbo_h_ = 0;

    // 高亮剪影 mask：单色（R8）纹理 + 独立 FBO（color-only），存被高亮物体的
    // 不扩张剪影（白=物体，黑=背景）。CPU 读回它做像素膨胀求恒定像素宽边缘环。
    unsigned int hl_mask_tex_ = 0, hl_mask_fbo_ = 0;
    int hl_mask_w_ = 0, hl_mask_h_ = 0;

    // Bloom（辉光）链：levels 级半分辨率 FBO + 纹理（大小 1/2, 1/4, 1/8, ...）。
    //   bloom_texs_[0] = 半分辨率：prefilter 高亮提取 + 上采样累加终点。
    //   bloom_texs_[1..] = 逐级减半的降采样/模糊级。
    //   bloom_work_tex_ = 与 HDR 同尺寸的临时纹理，存放加回辉光后的结果，
    //                    作为 tone map 的输入。
    // 生命周期随渲染尺寸变化（EnsureBloomChain 按需重建）。
    std::vector<unsigned int> bloom_texs_;
    std::vector<unsigned int> bloom_fbos_;
    std::vector<int> bloom_level_w_;
    std::vector<int> bloom_level_h_;
    unsigned int bloom_work_fbo_ = 0, bloom_work_tex_ = 0;
    int bloom_work_w_ = 0, bloom_work_h_ = 0;
    // 上采样累加后，最终辉光所在的 acc 纹理（L0 分辨率），供 composite 采样。
    unsigned int bloom_final_tex_ = 0;
    // 上采样累加用的乒乓缓冲（acc_a/acc_b 各半分辨率，随链一起分配）。
    unsigned int bloom_acc_a_ = 0, bloom_acc_b_ = 0;
    unsigned int bloom_acc_fbo_a_ = 0, bloom_acc_fbo_b_ = 0;

    unsigned int SolidProg();
    unsigned int TextProg();
    unsigned int ImageProg();
    unsigned int Solid3DProg();
    unsigned int Text3DProg();
    unsigned int DrawObject3DProg();
    unsigned int DrawObject3DProgFull();
    unsigned int ShadowProg();
    unsigned int TonemapProg();
    unsigned int PickProg();
    unsigned int BloomPrefilterProg();
    unsigned int BloomDownsampleProg();
    unsigned int BloomUpsampleProg();
    unsigned int BloomCompositeProg();

    TextureManager texture_mgr_;
    FontRenderer font_renderer_;
    MeshManager mesh_mgr_;
};

}  // namespace jpov

#endif

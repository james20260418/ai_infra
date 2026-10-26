// JPOV SSR（屏幕空间反射）子渲染器
//
// 定位：与粒子 / 体积火 / 半透明条带 / 远景雾**平级**的独立 pass，在
//   「不透明 3D 已画完」→ 半透明与雾之前执行，把场景内几何的倒影加法叠回 HDR。
//
// 它**不改任何既有 renderer 的 shader**：
//   - 反射体的表面属性由本模块自己的 attr pass 单独画一遍（只画被标记为反射体的物体，
//     `RenderCommandList::DrawObject3DReflective`），顶点着色器**复用**主 PBR 的那两份
//     （Object3DRenderer::kMeshVs3dPBR / kMeshVs3dPBRFull）⇒ 顶点变换逐字一致；
//   - 深度直接复用主 3D FBO 的深度纹理（挂到本模块的 FBO 上做 GL_LEQUAL 测试，
//     深度写关闭 ⇒ 不破坏主深度）。
//
// 为什么不做成「主 pass 多写一张 G-buffer（MRT）」：那要给 15 个 shader 加输出，
// 且任何一个漏写都会静默产出垃圾属性、症状只在倒影上体现。本模块的做法把改动
// 完全关在 src/ssr/ 里，既有 gold 一个都不动。
//
// 设计文档：tools/jpov/docs/jpov_ssr_design.md

#ifndef JPOV_SRC_SSR_SSR_RENDERER_H_
#define JPOV_SRC_SSR_SSR_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ssr_types.h"
#include "tools/jpov/src/mesh_manager.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

class SsrRenderer {
public:
    SsrRenderer() = default;
    ~SsrRenderer();

    SsrRenderer(const SsrRenderer&) = delete;
    SsrRenderer& operator=(const SsrRenderer&) = delete;

    // 编译 5 个 program + 建全屏 VAO。幂等。
    // Pre-condition: GL context 已激活；每个 Renderer 生命周期内调用一次。
    void Init(ShaderManager* shader_mgr);

    // 释放全部 GL 资源（纹理 / FBO / VAO）。幂等；可重复 Init。
    // Pre-condition: GL context 仍存活（必须在 Renderer 析构前调用）。
    void Finalize();

    // ---- 主入口 ----
    // 在「不透明 3D 已画完、半透明/雾之前」调用。内部顺序：
    //   ① 属性 pass（只画 ssr_reflect 标记的不透明 Object3D）
    //   ② Hi-Z 建设（4 级；level0 直接用传入的场景深度）
    //   ③ march（逐像素射线 → 反射缓冲）
    // 结果留在内部反射缓冲里，由 Composite() 叠进目标。
    //
    // 参数：
    //   cfg            : SSR 配置（总开关由调用方先判，本函数假定已 enabled）。
    //   proj / view    : 相机的投影 / 视图矩阵（列主序，与主 pass 同一套）。
    //   env_cube_tex   : 天空 cubemap 探针（未命中 / 粗糙面的兜底）；0 = 无探针。
    //   env_max_lod    : 探针 mip 上限（= log2(边长)）。
    //   scene_color_tex: 不透明 HDR 颜色（march 采它当命中色）。
    //   scene_depth_tex: 场景深度（**MRT#1 的 R32F**，值 = gl_FragCoord.z），march/Hi-Z 用。
    //   hw_depth_tex   : 硬件深度缓冲纹理（DEPTH_COMPONENT24），属性 pass 挂上做深度测试。
    //                    它与 scene_depth_tex 是两张不同的纹理，**不能混用**：
    //                    颜色格式（R32F）不能当 GL_DEPTH_ATTACHMENT 挂。
    //                    传 0 = 不做深度测试（属性缓冲可能被遮挡物覆盖，仅供调试）。
    //   fbo_w/fbo_h    : 主 3D FBO 尺寸（属性 pass 与深度都在这个分辨率）。
    // Pre-condition: scene_color_tex / scene_depth_tex 均非 0；cfg 参数合法。
    // Pre-condition: 调用返回时**不绑定任何 FBO**（调用方自己恢复）。
    void Draw(const RenderCommandList& cmds, const SsrConfig& cfg,
              MeshManager& mesh_mgr, TextureManager& texture_mgr,
              const Camera& cam, const float proj[16], const float view[16],
              unsigned int scene_color_tex, unsigned int scene_depth_tex,
              unsigned int hw_depth_tex, unsigned int env_cube_tex,
              float env_max_lod, int fbo_w, int fbo_h);

    // 把反射加法合成进**当前已绑定的**目标 FBO。
    // 只采样反射缓冲（它不是目标的附件）⇒ 无反馈环；用 GL_ONE/GL_ONE 加性混合。
    // Pre-condition: 目标 FBO 已绑定、viewport 已设、且**只声明了颜色附件 0**
    //   （否则未写的 MRT 附件内容未定义）；本函数内部会把 GL_BLEND 关回去。
    // Pre-condition: fbo_w/fbo_h 与 Draw() 传入的一致（只用来算 UV）。
    void Composite(int fbo_w, int fbo_h);

    // 最近一次 Draw 是否产出了反射缓冲（供调用方决定要不要 Composite）。
    bool HasResult() const { return refl_tex_ != 0; }

private:
    void EnsureBuffers(int fbo_w, int fbo_h, float scale);
    void DestroyBuffers();
    void EnsureHiZ(int level, int w, int h);
    void DestroyHiZ();

    void DrawAttributes(const RenderCommandList& cmds, MeshManager& mesh_mgr,
                        TextureManager& texture_mgr, const float view[16],
                        const float vp[16], int fbo_w, int fbo_h);
    void BuildHiZ(unsigned int scene_depth_tex, int fbo_w, int fbo_h);
    void March(const SsrConfig& cfg, const Camera& cam, const float proj[16],
               const float view[16], unsigned int scene_color_tex,
               unsigned int scene_depth_tex, unsigned int env_cube_tex,
               float env_max_lod, int fbo_w, int fbo_h);

    unsigned int AttrProg();
    unsigned int AttrFullProg();
    unsigned int HiZProg();
    unsigned int MarchProg();
    unsigned int CompositeProg();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int vao_ = 0;          // 空 VAO（全屏三角形也要绑 VAO 才合法）

    // ---- 属性缓冲（与主 3D FBO 同分辨率）：2 张 RGBA16F + 主深度 ----
    unsigned int attr_fbo_ = 0;
    unsigned int attr_normal_tex_ = 0;   // (Nv.xy, roughness, metallic)
    unsigned int attr_f0_tex_ = 0;       // (F0.rgb, 遮罩=1)
    int attr_w_ = 0, attr_h_ = 0;

    // ---- Hi-Z：level 0 = 传入的场景深度；这里只持有 1..4 ----
    // 用「每级一张独立纹理」而不是一张带 mip 的纹理：建塔时要"渲染进第 L 级、
    // 同时采样第 L-1 级"，若两者是同一张纹理就是 GL 的反馈环（未定义行为）。
    static constexpr int kHiZLevels = 4;     // 1..4
    unsigned int hiz_tex_[kHiZLevels] = {0, 0, 0, 0};
    unsigned int hiz_shared_fbo_ = 0;   // 各级共用一个 FBO，逐级重挂附件
    int hiz_w_[kHiZLevels] = {0, 0, 0, 0};
    int hiz_h_[kHiZLevels] = {0, 0, 0, 0};

    // ---- 反射缓冲（march 的输出，按 scale 降分辨率）----
    unsigned int refl_fbo_ = 0;
    unsigned int refl_tex_ = 0;
    int refl_w_ = 0, refl_h_ = 0;

    int progs_[5] = {0, 0, 0, 0, 0};   // 0=attr 1=attr_full 2=hiz 3=march 4=composite
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_RENDERER_H_

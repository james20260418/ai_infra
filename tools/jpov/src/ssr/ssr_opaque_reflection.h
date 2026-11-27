// JPOV SSR —— 不透明场景的接入层（属性 pass 的 program + 把装置接进主链路）
//
// ★ 分层（2026-11-27 重构后）：
//   SsrChain（ssr_chain.h）        —— **装置**：Source/Query → 全分辨率反射图。位置无关，
//                                    水皮等其它消费者直接用它，**不经过本类**。
//   SsrComposite（ssr_composite.h）—— 消费端：用 SSR 替换环境镜面项（"实心→实心"这件事
//                                    的位置知识：先减 env 再加 SSR）。
//   SsrOpaqueReflection（本类）    —— 胶水：编译属性 pass 的 program（Renderer 用自己的
//                                    draw 路径画它，见 DrawSsrAttrPass），每帧把
//                                    「3D FBO 的颜色/深度/属性纹理」翻译成 Source/Query，
//                                    调装置，再调合成写回 3D FBO。
//
// 管线位置（本类特有）：不透明 3D 之后、半透明（strip/粒子/雾）之前 ——
// 要的是"没有雾、没有半透明"的颜色（反射源）。
//
// Pre-condition：必须同时开启 env_specular（SSR 是那一项的**替换估计器**，没有 env 就无从替换）。

#ifndef JPOV_SRC_SSR_SSR_OPAQUE_REFLECTION_H_
#define JPOV_SRC_SSR_SSR_OPAQUE_REFLECTION_H_

#include <array>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_chain.h"
#include "tools/jpov/src/ssr/ssr_composite.h"

namespace jpov {

// 一帧的输入。全部只读。
struct SsrPassInputs {
    // 全分辨率（= 3D FBO 分辨率）的四张图
    unsigned int hdr_color_tex = 0;    // RGBA16F：不透明 3D 已画完的颜色
    unsigned int scene_depth_tex = 0;  // R32F：MRT#1 的 gl_FragCoord.z
    unsigned int attr_tex = 0;         // RGBA16F：rgb = 视空间法线，a = 掩码编码
    unsigned int attr_mat_tex = 0;     // RGBA16F：rgb = F0，a = roughness（逐像素材质）
    int full_w = 0;
    int full_h = 0;

    // 相机（针孔；与 3D pass 同一套）
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float tan_half_fov_y = 0.0f;
    float aspect = 0.0f;

    // 世界 → 视空间（3×3 旋转，列主序；与属性 pass 上传的 uWorldToView 同一份）。
    // 合成层要它的转置：属性纹理存的是视空间法线，而天球探针是世界方向的。
    // 零 = 没填，SsrComposite::Run 会 CHECK 打回。
    std::array<float, 9> world_to_view = {0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f};

    // 环境镜面（合成用它算"要被替换掉的那一份"）。与 3D pass 同一份 cubemap。
    unsigned int env_cube_tex = 0;
    float env_max_lod = 0.0f;
    float env_sky_scale = 1.0f;
    bool env_enabled = false;

    SsrConfig cfg;
};

// 生命周期：Init(shader_mgr) → 每帧 Render(inputs, dst_fbo) → Finalize()。
// Render 画进 dst_fbo 的 COLOR_ATTACHMENT0（= 3D FBO 的颜色）。
//
// 资源所有权（别的类容易在这里搞混，所以写死）：
//   · 本类**不拥有任何纹理**。输入的四张图、以及属性 pass 的 FBO / 属性纹理，全在
//     Renderer 手里（它们属于 3D pass，跨帧存在）；本类只借用。
//   · 本类只持有 GLSL program 的 id，而那些 program 归 `ShaderManager`（按名缓存，不删）。
//   · 真正的 GPU 资源（降采样/柱体/slice/mip 链/反射图）在 SsrChain / SsrComposite 里，
//     它们自己负责释放。
class SsrOpaqueReflection {
public:
    SsrOpaqueReflection();
    ~SsrOpaqueReflection();

    SsrOpaqueReflection(const SsrOpaqueReflection&) = delete;
    SsrOpaqueReflection& operator=(const SsrOpaqueReflection&) = delete;

    // 编译属性 pass 的 program，并初始化装置与合成。
    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一遍：属性纹理（已由 Renderer 画好）→ 装置 → 合成（写回 dst_fbo）。
    // Pre-condition: 输入纹理非 0 且尺寸 == full_w×full_h；cfg.enabled。
    void Render(const SsrPassInputs& in, unsigned int dst_fbo);

    // 属性 pass 的 program（Renderer 用自己的 draw 路径调它，见 DrawSsrAttrPass）。
    //   attr_program()         ：object3d 不透明（kSsrAttrVs + kSsrAttrFs）
    //   attr_cutout_program()  ：object3d 镂空（VS 带 UV，FS 采样 baseColor.a 后 discard）
    //   attr_skinned_program() ：蒙皮（复用 kSkinnedVs + kSsrAttrSkinnedFs，曲面镜面用）
    unsigned int attr_program() const { return prog_attr_; }
    unsigned int attr_cutout_program() const { return prog_attr_cutout_; }
    unsigned int attr_skinned_program() const { return prog_attr_skinned_; }

private:
    ShaderManager* shader_mgr_ = nullptr;

    unsigned int prog_attr_ = 0;          // 属性 pass（Renderer 用；这里只管编译）
    unsigned int prog_attr_cutout_ = 0;   // 属性 pass 的镂空变体
    unsigned int prog_attr_skinned_ = 0;  // 蒙皮变体（VS 复用 kSkinnedVs）

    SsrChain chain_;          // 装置（位置无关）
    SsrComposite composite_;  // 消费端（替换环境镜面项）

    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_OPAQUE_REFLECTION_H_

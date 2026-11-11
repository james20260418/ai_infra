// JPOV SSR 接入层 —— 把 SSR 接进主 pass 的那一层（编排，不含算法）
//
// 分工（三层，各自可独立替换/测试）：
//   SsrDownsample（ssr_downsample.h）：**输入准备** —— 全分辨率 (color,depth,attr) 降到 SSR
//       分辨率（N×N 选点）。纯数学部分 GL-free，单测在 ssr_downsample_test.cc。
//   SsrRenderer  （ssr_renderer.h）：**算法** —— 纯模块，同分辨率进/出：
//       radiance(rgb) + ray_ratio(a) + 类型图（命中/天空/出屏/预算耗尽）。
//   SsrFilter    （ssr_filter.h）：**粗糙度预滤波** —— 把 radiance/type 变成
//       覆盖率加权的 mip 链（A 预乘 + 空像素填充），合成层按 roughness 采 textureLod。
//       没有这一步，粗糙材质上的命中区域仍是像素级锐利（"把石头画成镜子"）。
//   SsrFusion    （ssr_fusion.h）：**融合** —— A 预乘双边升采样 + 用 SSR **替换**
//       3D pass 已经加过的那一份环境镜面项（不是叠加，叠加会让金属亮一倍）：
//           out = hdr + mask·A·ao·(ssr_L·weight − env_term)
//       A = 0 ⇒ 整项抵消 ⇒ 与"没开 SSR"逐字节相同（单调改进）。
//       材质（F0/roughness）与模糊量都**逐像素**取自属性纹理附件 1。
//   SsrIntegration（本类）：**编排** —— HDR 副本 blit → 调上面四者；另外编译属性 pass 的
//       program（Renderer 用自己的 draw 路径画它，见 DrawSsrAttrPass）。
//
// 管线位置：不透明 3D 之后、半透明（strip/粒子/雾）之前 —— 要的是"没有雾、没有半透明"的颜色。
//
// Pre-condition：必须同时开启 env_specular（SSR 是那一项的**替换估计器**，没有 env 就无从替换）。

#ifndef JPOV_SRC_SSR_SSR_INTEGRATION_H_
#define JPOV_SRC_SSR_SSR_INTEGRATION_H_

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_downsample.h"
#include "tools/jpov/src/ssr/ssr_fusion.h"
#include "tools/jpov/src/ssr/ssr_filter.h"
#include "tools/jpov/src/ssr/ssr_renderer.h"

namespace jpov {

// 一帧的输入。全部只读。
struct SsrPassInputs {
    // 全分辨率（= 3D FBO 分辨率）的三张图
    unsigned int hdr_color_tex = 0;    // RGBA16F：不透明 3D 已画完的颜色
    unsigned int scene_depth_tex = 0;  // R32F：MRT#1 的 gl_FragCoord.z
    unsigned int attr_tex = 0;         // RGBA16F：rgb = 视空间法线，a = 掩码编码
    unsigned int attr_mat_tex = 0;     // RGBA16F：rgb = F0，a = roughness（逐像素材质）
    unsigned int attr_slab_tex = 0;    // RG32F：体素柱 [front, back]（w = 1/z_view，0/0 = 空格）
    int full_w = 0;
    int full_h = 0;

    // 相机（针孔；与 3D pass 同一套）
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float tan_half_fov_y = 0.0f;
    float aspect = 0.0f;

    // 反射层材质在 SsrConfig 里（base_color / metallic / roughness / ao）；
    // **本版只支持"所有反射体共用一套材质"**（见 ssr_fusion.h 的"已知限制"）。

    // 环境镜面（融合用它算"要被替换掉的那一份"）。与 3D pass 同一份 cubemap。
    unsigned int env_cube_tex = 0;
    float env_max_lod = 0.0f;
    float env_sky_scale = 1.0f;
    bool env_enabled = false;

    SsrConfig cfg;
};

// 生命周期：Init(shader_mgr) → 每帧 Render(inputs, dst_fbo) → Finalize()。
// Render 画进 dst_fbo 的 COLOR_ATTACHMENT0（= 3D FBO 的颜色），返回前恢复 GL 状态。
class SsrIntegration {
public:
    SsrIntegration();
    ~SsrIntegration();

    SsrIntegration(const SsrIntegration&) = delete;
    SsrIntegration& operator=(const SsrIntegration&) = delete;

    // 编译属性 pass 的 program，并初始化三个子模块。
    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一遍：HDR 副本 → 降采样 → SSR 模块 → 融合（写回 dst_fbo）。
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
    void EnsureCopyTarget(int w, int h);

    ShaderManager* shader_mgr_ = nullptr;

    unsigned int prog_attr_ = 0;          // 属性 pass（Renderer 用；这里只管编译）
    unsigned int prog_attr_cutout_ = 0;   // 属性 pass 的镂空变体
    unsigned int prog_attr_skinned_ = 0;  // 蒙皮变体（VS 复用 kSkinnedVs）

    unsigned int copy_tex_ = 0;   // 全分辨率 HDR 副本（避免反馈环）
    unsigned int copy_fbo_ = 0;
    int width_ = 0;
    int height_ = 0;

    SsrDownsample downsample_;
    SsrRenderer ssr_;
    SsrFilter filter_;
    SsrFusion fusion_;

    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_INTEGRATION_H_

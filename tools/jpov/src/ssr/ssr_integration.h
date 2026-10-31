// JPOV SSR 接入层 —— 降采样 / SSR / 升采样合成（把 SsrRenderer 接进主 pass）
//
// 与 src/ssr/ssr_renderer.h 的分工：
//   SsrRenderer        ：纯模块（给定分辨率的 color/depth/normal → 同分辨率的 radiance + A）
//   SsrIntegration（本类）：把模块接进管线的那一层 —— 全分辨率降采样、调用模块、
//                        再把低分辨率结果用 A 预乘的双边核升采样、替换掉 3D pass 已经
//                        加过的那一份环境镜面项。
//
// 为什么合成写成"替换"而不是"叠加"：
//   3D pass 里每个像素已经加过 `(F0·AB.x+AB.y)·ao·EnvSpecularTerm(...)`（PR #228）。
//   SSR 要做的是**替换**这一项的估计器，不是再加一项，否则金属会亮一倍。
//   所以合成 = `hdr + mask·A·ao·(ssr_L·weight − env_term)`：
//     · A = 1 ⇒ 整项换成 SSR 的辐亮度
//     · A = 0 ⇒ env_term − env_term = 0，画面与今天**逐字节相同**（单调改进的保证）
//
// 降采样按"选点"（2×2 里挑一个 texel 代表整组）：
//   · color / depth / normal **全部取自同一个 texel** —— 跨 texel 混（颜色平均 + 深度取最小）
//     会让颜色和深度分属两个不同表面，出错极隐蔽。
//   · 挑**最近**（min 窗口深度）的那个：深度缓冲只记最前表面，镜子里能看见的也是它；
//     被挡住的背景不该发射射线。min 对连续函数连续 ⇒ 相机移动时不会跳变。

#ifndef JPOV_SRC_SSR_SSR_INTEGRATION_H_
#define JPOV_SRC_SSR_SSR_INTEGRATION_H_

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_renderer.h"

namespace jpov {

// 一帧的输入。全部只读。
struct SsrPassInputs {
    // 全分辨率（= 3D FBO 分辨率）的三张图
    unsigned int hdr_color_tex = 0;    // RGBA16F：不透明 3D 已画完的颜色
    unsigned int scene_depth_tex = 0;  // R32F：MRT#1 的 gl_FragCoord.z
    unsigned int attr_tex = 0;         // RGBA16F：.xy = 视空间法线，.a = 1（是反射体）
    int full_w = 0;
    int full_h = 0;

    // 相机（针孔；与 3D pass 同一套）
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float tan_half_fov_y = 0.0f;
    float aspect = 0.0f;

    // 反射层材质在 SsrConfig 里（base_color / metallic / roughness / ao）；
    // **本版只支持"所有反射体共用一套材质"**（见 .cc 顶部"已知限制"）。

    // 环境镜面（composite 用它算"要被替换掉的那一份"）。与 3D pass 同一份 cubemap。
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

    // 编译降采样 / 合成两个 program，并把 SsrRenderer 初始化好。
    // Pre-condition: GL context 已激活。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一遍：降采样 → SSR → 合成（写回 dst_fbo）。
    // Pre-condition: 输入纹理非 0 且尺寸 == full_w×full_h；cfg.enabled。
    void Render(const SsrPassInputs& in, unsigned int dst_fbo);

    // 属性 pass 的 program（Renderer 用自己的 draw 路径调它，见 DrawSsrAttrPass）。
    //   attr_program()        ：object3d（kSsrAttrVs + kSsrAttrFs）
    //   attr_skinned_program()：蒙皮（复用 kSkinnedVs + kSsrAttrSkinnedFs，曲面镜面用）
    unsigned int attr_program() const { return prog_attr_; }
    unsigned int attr_skinned_program() const { return prog_attr_skinned_; }

    // 最近一次 SSR 的分辨率（供 HUD / 测试读数）。
    int ssr_width() const { return ssr_w_; }
    int ssr_height() const { return ssr_h_; }

private:
    void EnsureTargets(int w, int h, int ssr_w, int ssr_h, bool coverage_debug, bool attr_valid);
    void DestroyTargets();

    ShaderManager* shader_mgr_ = nullptr;

    unsigned int prog_attr_ = 0;         // 属性 pass（Renderer 用；这里只管编译）
    unsigned int prog_attr_skinned_ = 0; // 属性 pass 的蒙皮变体（VS 复用 kSkinnedVs）
    unsigned int prog_downsample_ = 0;   // 全分辨率 → SSR 分辨率（选点）
    unsigned int prog_composite_ = 0;    // 升采样 + 替换环境镜面项

    unsigned int copy_tex_ = 0;          // 全分辨率 HDR 副本（避免反馈环）
    unsigned int copy_fbo_ = 0;
    unsigned int down_fbo_ = 0;
    unsigned int down_color_tex_ = 0;    // RGBA16F @ SSR 分辨率
    unsigned int down_depth_tex_ = 0;    // R32F
    unsigned int down_normal_tex_ = 0;   // RGBA16F（rgb = 三维视空间法线，a = 掩码编码）
    unsigned int dst_fbo_ = 0;           // 合成的目标（外部传入，仅记录用于恢复）

    SsrRenderer ssr_;

    int width_ = 0;
    int height_ = 0;
    int ssr_w_ = 0;
    int ssr_h_ = 0;
    bool coverage_debug_ = false;
    bool attr_valid_ = true;
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_INTEGRATION_H_

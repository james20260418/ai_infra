// JPOV SSR — 融合（升采样 + 替换环境镜面项）
//
// 职责：把 SSR 模块的低分辨率结果按**A 预乘的双边升采样**放回全分辨率，并用它**替换**
// 3D pass 已经加过的那一份环境镜面项（不是叠加 —— 叠加会让金属亮一倍）。
//
//   out = hdr + mask · A · ao · (ssr_L · weight − env_term)
//
//   mask     = 反射体（属性纹理 .a > 0.75）
//   A        = 本像素 SSR 是否有可用结果（0 ⇒ 整项抵消，与"没开 SSR"逐字节相同）
//   weight   = F0·AB.x + AB.y（EnvBrdfApprox）
//   env_term = EnvSpecularTerm(...)（与 3D pass 同一份实现，见 env_specular.h）
//
// 已知限制（本版，写清楚免得当成 bug）：
//   · **反射体只支持"全场景一套材质"**（base_color/metallic/roughness/ao 是 uniform）。
//     属性纹理只写视空间法线，不写逐像素材质。升级点：把 F0/roughness 放进属性纹理的
//     额外附件即可，接口不变（现在故意不做）。
//   · **AO 不逐像素**（用 uniform，默认 1）。
//   · **粗糙度不做 LOD**：只对 roughness ≈ 0 的反射体语义正确。
//
// 升采样的值域项见 SsrConfig 的 upsample_* 注释：连续软权 + 权重下限，
// 外加"1/z 对屏幕坐标仿射"的预测式深度判据（根治掠射条纹）。

#ifndef JPOV_SRC_SSR_SSR_FUSION_H_
#define JPOV_SRC_SSR_SSR_FUSION_H_

#include "tools/jpov/interface/render_command.h"

namespace jpov {

class ShaderManager;

// 一趟融合的全部输入（只读）。
struct SsrFusionInputs {
    // 全分辨率
    unsigned int hdr_copy_tex = 0;     // RGBA16F：3D 不透明 pass 的颜色副本
    unsigned int attr_tex = 0;         // RGBA16F：rgb = 视空间法线，a = 掩码编码
    unsigned int scene_depth_tex = 0;  // R32F：MRT#1 的窗口深度
    int full_w = 0;
    int full_h = 0;

    // SSR 分辨率（模块输出 + 降采样输入）
    unsigned int ssr_radiance_tex = 0;  // RGBA16F：rgb = 命中颜色，a = ray_ratio
    unsigned int ssr_type_tex = 0;      // R8UI：命中/逃逸类型（见 ssr_renderer.h）
    unsigned int down_depth_tex = 0;    // R32F
    unsigned int down_normal_tex = 0;   // RGBA16F
    int ssr_w = 0;
    int ssr_h = 0;
    int scale = 1;                      // = 1 / resolution_scale（1..4）

    // 相机（与 3D pass 同一套）
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float tan_half_fov_y = 0.0f;
    float aspect = 0.0f;

    // 环境镜面（替换对象）
    unsigned int env_cube_tex = 0;
    float env_max_lod = 0.0f;
    float env_sky_scale = 1.0f;
    bool env_enabled = false;

    // 材质 + 升采样参数 + 调试开关
    SsrConfig cfg;

    // 写回目标（3D FBO 的 COLOR_ATTACHMENT0）
    unsigned int dst_fbo = 0;
};

// 生命周期：Init(shader_mgr) → 每帧 Run(inputs) → Finalize()。
// Run 会保存/恢复调用方的 FBO、viewport、程序、活动纹理单元与纹理绑定。
class SsrFusion {
public:
    SsrFusion();
    ~SsrFusion();
    SsrFusion(const SsrFusion&) = delete;
    SsrFusion& operator=(const SsrFusion&) = delete;

    void Init(ShaderManager* shader_mgr);
    void Finalize();
    void Run(const SsrFusionInputs& in);

private:
    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_FUSION_H_

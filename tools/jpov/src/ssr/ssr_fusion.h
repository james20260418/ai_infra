// JPOV SSR — 融合（升采样 + 替换环境镜面项）
//
// 职责：把 SSR 模块的低分辨率结果按**A 预乘的双边升采样**放回全分辨率，并用它**替换**
// 3D pass 已经加过的那一份环境镜面项（不是叠加 —— 叠加会让金属亮一倍）。
//
//   out = hdr + mask · A · ao · (ssr_L · weight − env_term)
//             − mask · ao · bf · (1−A) · (1−k) · env_term
//
//   （第二行就是“背面命中时把 remainder env 压暗”；bf = 背面标记，空间插值后 ∈ [0,1]）
//
//   mask     = 反射体（属性纹理附件0的 .a > 0.75）
//   A        = 预滤波 mip 链里**该 roughness 对应的那一级**的覆盖率（0 ⇒ 整项抵消，
//              与"没开 SSR"逐字节相同）
//   ssr_L    = 同一级的覆盖率加权平均辐亮度
//   weight   = F0·AB.x + AB.y（EnvBrdfApprox）
//   env_term = EnvSpecularTermF0(...)（与 3D pass 同一份实现，见 env_specular.h）
//   k        = backface_env_scale：**背面命中**（射线穿过了一个屏幕里看不见背面的物体）
//              且没有合法命中时，剩余那份 env 的乘子。1 = 旧行为；0 = 那份 env 全黑。
//              只吃 (1−A) 的份额（A=1 时命中的像素不受影响），且只压环境镜面项；
//              bf=0（默认）时下面直接走旧算式 ⇒ 逐字节不变。
//
// 2026-11-02：材质与模糊都改成**逐像素** ——
//   · F0/roughness 来自属性纹理**附件 1**（rgb = F0，a = roughness），不再用一套 uniform
//     材质（旧做法的直接后果：同屏两种反射材质必然算错，10-31 铬人腰部蓝光）。
//   · ssr_L/A 来自 `SsrFilter` 的 A 预乘 mip 链，LOD = roughness² · roughness_to_lod：
//     粗糙度连续变化 ⇒ 反射连续变糊（"逐渐磨砂"），并且在覆盖率掉光处自然交回 env。
//
// 已知限制（本版，写清楚免得当成 bug）：
//   · **AO 不逐像素**（用 uniform，默认 1；本管线没有 SSAO）。
//   · **模糊半径不随距离补偿**：屏幕空间 mip 的角分辨率在远处偏大（远景倒影糊得比
//     物理上该有的更多）。要修需要"到反射面的距离"，那要 SSR 输出额外的命中距离图。
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
    unsigned int attr_mat_tex = 0;     // RGBA16F：rgb = F0，a = roughness（逐像素材质）
    unsigned int scene_depth_tex = 0;  // R32F：MRT#1 的窗口深度
    int full_w = 0;
    int full_h = 0;

    // SSR 分辨率（模块输出 + 降采样输入）
    // R8UI，SSR 分辨率：bit0 = 背面命中见过（融合层据此压 env fallback）。
    // 与 type 正交，所以不挤进 type 枚举（见 ssr_shader.h 的 resolve）。
    unsigned int ssr_flags_tex = 0;
    unsigned int down_w_tex = 0;        // R32F：w = 1/z_view（0 = 空）
    unsigned int down_normal_tex = 0;   // RGBA16F
    // 粗糙度预滤波结果（SsrFilter::mip_tex()）：RGBA16F + 完整 mip 链。
    unsigned int ssr_mip_tex = 0;
    int ssr_mip_levels = 0;
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

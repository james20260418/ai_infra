// JPOV SSR —— 不透明场景的**消费端**：用 SSR 替换环境镜面项
//
// ⚠️ 这个类**不是** SSR 装置的一部分（装置在 ssr_chain.h）。它属于"实心→实心"这个
//    **具体位置**的知识，刻意留在装置外面：
//      · 它知道"3D pass 已经把环境镜面项加进 HDR 了"，所以要先减掉（替换，不是叠加）；
//      · 它知道材质在属性纹理附件 1（F0 / roughness）里、AO 是全局常量；
//      · 它知道探针是世界方向的 cubemap，而属性纹理存的是视空间法线。
//    水皮/其它消费者不要复用这里任何一条 —— 它们只要 SsrChain 的结果，然后用**自己的**
//    材质/介质模型去混。
//
// 合成公式（单调改进：A = 0 ⇒ 整项抵消 ⇒ 与"没开 SSR"逐字节相同）：
//
//     out = hdr − mask·A_rm·ao·env_term + mask·A_ad·ao·ssr_L·weight
//     weight   = F0·EnvBrdfApprox(rough).x + EnvBrdfApprox(rough).y
//     env_term = EnvSpecularTermF0(n_world, V_world, rough, F0)   （与 3D pass 同一份实现）
//
//   先"减掉 3D pass 已加的那一份环境镜面"，再加上 SSR 的估计 —— 叠加会让金属亮一倍。
//   `removed` 逐通道钳到 hdr 以上（辐亮度不可能为负）：剪影上属性 pass 与主 pass 的
//   1~3 px 覆盖差异会让 env_term 大于 hdr 里实际存在的那一份，不钳就是纯红描边。

#ifndef JPOV_SRC_SSR_SSR_COMPOSITE_H_
#define JPOV_SRC_SSR_SSR_COMPOSITE_H_

#include <array>

namespace jpov {

class ShaderManager;

// 一趟合成的全部输入（只读）。
struct SsrCompositeInputs {
    // ---- 全分辨率输入 ----
    unsigned int hdr_tex = 0;          // RGBA16F：3D 不透明 pass 的颜色（**要被写回**）
    unsigned int attr_tex = 0;         // RGBA16F：rgb = 视空间法线，a = 掩码编码
    unsigned int attr_mat_tex = 0;     // RGBA16F：rgb = F0，a = roughness（逐像素材质）
    unsigned int scene_depth_tex = 0;  // R32F：窗口深度（算视方向 V 要用）
    // SsrChain 的输出（ssr_chain.h::SsrChainResult）
    unsigned int reflection_tex = 0;   // RGBA32F：rgb = 辐亮度 L，a = 覆盖率 A
    unsigned int backface_tex = 0;     // R8UI：bit0 = 背面命中
    unsigned int raw_ssr_tex = 0;      // SSR 分辨率 RGBA16F（**仅调试档 4**）
    int full_w = 0;
    int full_h = 0;
    int ssr_w = 0;                     // raw_ssr_tex 的尺寸（调试档 4 用）
    int ssr_h = 0;
    int scale = 0;                     // 全分辨率 / SSR 分辨率

    // ---- 相机 ----
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float tan_half_fov_y = 0.0f;
    float aspect = 0.0f;

    // ---- 世界 → 视空间（3×3 旋转，列主序）----
    // 环境探针按**世界方向**采样，而属性纹理存的是**视空间**法线 ⇒ 必须转回去，
    // 否则减掉的不是 3D pass 加进去的那一份（2026-11-24 修"暗物体倒影被天光洗白"）。
    // 全 0 = 没填 ⇒ Run 直接 FATAL。
    std::array<float, 9> world_to_view = {0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f};

    // ---- 环境镜面（被替换的那一项）----
    unsigned int env_cube_tex = 0;
    float env_max_lod = 0.0f;
    float env_sky_scale = 1.0f;
    bool env_enabled = false;

    // ---- 材质无关的全局旋钮 + 背面命中的处置（两个正交 flag）----
    float ao = 1.0f;               // 全局 AO（本管线没有 SSAO）
    bool backface_use_ssr = false; // false = 交回 env（默认，与旧行为逐字节相同）
    float backface_dim = 1.0f;     // 该像素最终采用的那份颜色的亮度因子，1 = 不压暗
    int debug_view = 0;            // 0 = 正常；1 = A 灰度；2 = 直通；3 = SSR 辐亮度；4 = A(SSR 档)；5 = 背面掩码

    // ---- 写回目标（3D FBO 的 COLOR_ATTACHMENT0）----
    unsigned int dst_fbo = 0;
};

// 生命周期：Init(shader_mgr) → 每帧 Run(inputs) → Finalize()。
// Run 会保存/恢复调用方的 FBO、viewport、program、活动纹理单元与纹理绑定。
class SsrComposite {
public:
    SsrComposite();
    ~SsrComposite();
    SsrComposite(const SsrComposite&) = delete;
    SsrComposite& operator=(const SsrComposite&) = delete;

    void Init(ShaderManager* shader_mgr);
    void Finalize();
    void Run(const SsrCompositeInputs& in);

private:
    // HDR 副本：合成要**读** hdr 又**写** hdr（同一张纹理 = 反馈环）⇒ 先 blit 一份。
    void EnsureCopyTarget(int w, int h);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
    unsigned int copy_tex_ = 0;
    unsigned int copy_fbo_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_COMPOSITE_H_

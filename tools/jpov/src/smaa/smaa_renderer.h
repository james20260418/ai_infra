// JPOV SmaaRenderer —— 屏幕空间抗锯齿 SMAA 1x（后处理 pass，与 bloom / tone map 平级）
//
// 定位（Danis 2026-10-14 定）：**3D 后处理链的最后一站**。tone map 把 HDR 压到 LDR 后，
// 本渲染器在**输出（viewport）分辨率**下做 SMAA 1x，得到抗锯齿后的最终 LDR 图像。
//
// ⭐ 为什么放在 tone map **之后**、且在**输出分辨率**下（而不是 3D FBO 分辨率）：
//   · SMAA 是**逐像素**的边缘导向 AA，边缘检测在最终灰度（gamma 空间）上做才有意义；
//     放 tone map 之前（HDR）会失去「LDR 感知亮度」这一前提。
//   · 3D FBO 尺寸（fbo_3d_width_/height_）与屏幕（viewport）尺寸**可以不同**，二者之间
//     那一步「blit / tone map 重采样」正是**分辨率改变**发生的地方。若在 3D FBO 分辨率下
//     做 AA 再重采样到屏幕，AA 结果会被二次过滤、且与最终像素栅格错位 —— 收益打折。
//     故本渲染器一律在**输出分辨率**下工作：tone map 先把场景写进本渲染器的 LDR 场景 FBO
//     （viewport 尺寸），SMAA 三趟都跑在该分辨率，最后一趟直接把结果写进屏幕主 FBO。
//
// 三趟（见 smaa_shader.h）：luma 边检测 → 混合权重（查 area/search LUT）→ 邻域混合。
// 预算的 LUT 纹理内嵌在 smaa_area_tex.h / smaa_search_tex.h（参考实现的 MIT 预计算数据）。
//
// 生命周期：Init（编译 program + 上传 LUT）→ 每帧 EnsureSceneFBO + Apply → Finalize。
// 不启用 SMAA 时（Renderer 侧）本渲染器完全不被触碰，零开销、零回归。

#ifndef JPOV_SRC_SMAA_SMAA_RENDERER_H_
#define JPOV_SRC_SMAA_SMAA_RENDERER_H_

#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class SmaaRenderer {
public:
    SmaaRenderer();
    ~SmaaRenderer();

    SmaaRenderer(const SmaaRenderer&) = delete;
    SmaaRenderer& operator=(const SmaaRenderer&) = delete;

    // 编译三趟 program + 上传两张 LUT 纹理（area 160x560 RG8 / search 64x16 R8）。
    // Pre-condition: GL context 已激活（Renderer::Init 内调用）。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 确保 LDR「场景」FBO（SMAA 输入）存在，尺寸 = 输出分辨率 w×h。幂等。
    // 返回后调用方应把 tone map 的绘制目标指向本 FBO，并把 viewport 设为 (0,0,w,h)。
    void EnsureSceneFBO(int w, int h);
    unsigned int scene_fbo() const { return scene_fbo_; }
    unsigned int scene_tex() const { return scene_tex_; }

    // 跑 SMAA 三趟：读 scene_tex()（tone map 已写入），把抗锯齿后的结果写进
    // target_fbo 的视口矩形 (vp_x, vp_y, w, h)。
    // Pre-condition: Init 已调用；EnsureSceneFBO(w,h) 已调用且 scene 纹理已写入。
    //   w,h > 0；w,h <= Renderer::kMaxFboDim。
    void Apply(float threshold, int w, int h,
               unsigned int target_fbo, int vp_x, int vp_y);

private:
    void EnsureEdgesFBO(int w, int h);
    void EnsureWeightsFBO(int w, int h);
    void DestroyTargets();
    void UploadLookupTextures();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_edge_ = 0;     // 第 1 趟：luma 边检测
    unsigned int prog_weight_ = 0;   // 第 2 趟：混合权重计算
    unsigned int prog_blend_ = 0;    // 第 3 趟：邻域混合

    unsigned int area_tex_ = 0;      // AreaTex（RG8，160x560）
    unsigned int search_tex_ = 0;    // SearchTex（R8，64x16）

    unsigned int scene_fbo_ = 0, scene_tex_ = 0;   // LDR 场景（SMAA 输入/输出底色）
    unsigned int edges_fbo_ = 0, edges_tex_ = 0;   // edgesTex（RGBA16F）
    unsigned int weights_fbo_ = 0, weights_tex_ = 0;  // blendTex（RGBA16F）
    int tex_w_ = 0, tex_h_ = 0;      // 三张内部纹理共用尺寸

    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_SMAA_SMAA_RENDERER_H_

// JPOV WaterSkinRenderer — 水皮子渲染器（froxel 之后的一次独立 pass）
//
// 角色：把「水面」画成一块**水平正方形 quad**（默认 10 km²，与 strip 平铺版的尺寸一致），
// 与帧缓冲里已有的不透明场景做**简单 alpha 混合**（透射 = out = water·α + dst·(1−α)）。
// 与 SkyRenderer / HorizonFogRenderer / FireFogRenderer 在 renderer 层面平级。
//
// 调用时机（见 Renderer::Render）：froxel（点状雾）合成之后、highlight / tone map 之前。
// 不持有自己的 FBO：就地混合到调用方当前绑定的 3D HDR FBO（要求 tone_mapping = true）。
//
// ⚠️ 本版**不进 RenderCommandList**：参数（WaterSkinParams）由 demo 直接设在 Renderer 上
//    （JPOV::SetWaterSkin）。观感定下来之后再把形状收进 interface（与 strip 版同构的
//    WaterSurface），那时这个直连口子要一并删掉。
//
// 已知边界（有意不做，见 water_skin_shader.h 末尾）：无吸收/散射、无反射探针、无波浪。

#ifndef JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_
#define JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/pbr_material.h"   // Color
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

// 水皮的**实验期参数**（直连 Renderer，暂不进 RenderCommandList）。
//
// 这里刻意只有"表面光学"的旋钮：几何永远是 y = level_y 的正方形平面。
// 没有字段带隐式特殊值 —— half_extent_m 必须 > 0（为 0 时 Draw 会 CHECK 崩），
// 想关掉水皮就把整个 optional 设成 nullopt（JPOV::SetWaterSkin(std::nullopt)）。
struct WaterSkinParams {
    float level_y = 0.0f;         // 水面世界高度（米，y-up）
    float half_extent_m = 0.0f;   // 正方形半边长（米），必须 > 0
    // 本体色 + 正对视线时的覆盖率：α = 0.10 ⇒ 一眼看穿，只有一层很淡的色偏。
    Color body_color{0.62f, 0.80f, 0.86f, 1.0f};
    float body_alpha = 0.10f;
    // 菲涅耳（Schlick）：f0 = 0.02 是水的正入射反射率；power = 5 是物理指数。
    float fresnel_f0 = 0.02f;
    float fresnel_power = 5.0f;
    // 掠射处收敛到的反射色（本版是常量，不采天空 cubemap）。
    Color rim_color{0.80f, 0.90f, 1.00f, 1.0f};
};

class WaterSkinRenderer {
public:
    // Init: 建 quad 的 VAO/VBO（局部 ±1 的正方形，展开由 uniform 做）。
    // Pre-condition: 已创建 GL context；Finalize 后不可再次 Draw（可重新 Init）。
    void Init();
    void Finalize();

    // Draw: 把水皮混合到**当前绑定的 FBO**（调用方负责绑定 FBO / viewport / draw buffer）。
    //   params            : 水面高度、半边长、表面光学参数。
    //   cam               : 相机（取位置与 near/far 做深度线性化）。
    //   view_proj         : 相机 Proj*View（列主序 16 float）。
    //   scene_depth_tex   : MRT#1 场景深度（R32F 单采样）。必须**不**是当前 draw FBO 的
    //                       附件（否则形成采样↔写入的反馈环）。
    //   width / height    : 3D FBO 尺寸（把 gl_FragCoord 换成 [0,1] uv）。
    // Pre-condition: params.half_extent_m > 0；scene_depth_tex != 0。
    // 返回后 GL_BLEND / GL_DEPTH_TEST / GL_CULL_FACE 均为关（与 3D pass 结束后的状态一致）。
    void Draw(const WaterSkinParams& params, const Camera& cam, const float view_proj[16],
              unsigned int scene_depth_tex, int width, int height, ShaderManager& shader_mgr);

private:
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

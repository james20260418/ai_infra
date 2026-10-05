// JPOV HorizonFogRenderer — 远景仰角雾（空气透视）渲染器
//
// 一个独立的全屏后处理子渲染器（与 SkyRenderer / Bloom 等在 renderer 层面平级）。
// 它**不持有自己的 FBO**：由固定管线 alpha 混合就地合成到**调用方当前绑定的**
// 3D HDR FBO 上（雾用到的场景颜色就是该 FBO 里已有的像素，无需再采样）。
//
// 调用时机（见 Renderer::Render）：3D 绘制全部完成之后、HDR 后处理（highlight /
// bloom / tone map）之前 —— 即「其它 3D 渲染之后、HDR 前」。要求 tone_mapping=true。

#ifndef JPOV_SRC_HORIZON_FOG_HORIZON_FOG_RENDERER_H_
#define JPOV_SRC_HORIZON_FOG_HORIZON_FOG_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class HorizonFogRenderer {
public:
    // 全屏雾 pass：绘制到「当前绑定的 FBO」（就地 alpha 混合，不持有自己的 FBO）。
    //   cfg               : 雾参数（距离/仰角带/浓度/雾色）。
    //   scene_depth_tex   : MRT#1 场景深度（R32F，单采样）。必须**不**是当前 draw FBO
    //                       的附件（否则形成采样↔写入反馈环）。
    //   sky_color_tex     : 大气色天空纹理（收敛色=该方向天空色）。0 或无天空时退回 cfg.color。
    //   view_proj         : 相机 Proj*View（列主序 16 float），内部求逆得逆 VP。
    //   cam               : 相机（取世界位置算到可见面的距离）。
    //   shader_mgr        : shader 缓存。
    // 前置：目标 FBO 已绑定、viewport 已设；调用返回后 GL_BLEND/GL_DEPTH_TEST/GL_CULL_FACE
    //       均被置为「关」（与 3D pass 结束后的状态一致）。
    static void Draw(const ElevationFogConfig& cfg,
                     unsigned int scene_depth_tex,
                     unsigned int sky_color_tex,
                     const float view_proj[16],
                     const Camera& cam,
                     ShaderManager& shader_mgr);
};

}  // namespace jpov

#endif  // JPOV_SRC_HORIZON_FOG_HORIZON_FOG_RENDERER_H_

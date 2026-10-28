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
#include "tools/jpov/interface/render_command.h" // DirectionalLight
#include "tools/jpov/src/water/water_normal_tile.h" 
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
    // 掠射处收敛到的反射色（本版是常量，不采天空 cubemap）。
    Color rim_color{0.80f, 0.90f, 1.00f, 1.0f};

    // ── 水下视角（相机在 level_y 以下时才生效；判定在 FS 里用 uCamPos.y 自己做）──
    // 临界角外是全内反射 —— 看到的其实是**水下环境**，本版用这个"地色"代表。
    // 典型取值 = 天光推导的地色（SkyCommand::AmbientTricolor()[2]），与天光同源。
    // ⚠️ 必须取**辐亮度**量级的色（SkyCommand::ground_color，例如 0.05/0.06/0.08），
    //    不要用天光推导的 AmbientTricolor()[2]（那是"环境光量级"、还被均值归一过 ⇒
    //    填充色比窗内的天空还亮，画面上会变成一圈亮灰雾，与真实"窗外是暗镜"相反）。
    Color tir_color{0.05f, 0.06f, 0.08f, 1.0f};
    // 水的折射率：临界角 = asin(1 / ior) ≈ 48.6°（1.333）。只用于 Fresnel/TIR 的界面权重
    // （**不做折射查找**，见 water_skin_shader.h 的说明）。
    float ior = 1.333f;
    // **实验旋钮**：强制最小透射 [0,1]。0 = 物理（TIR 区 R=1、整片地色）；
    // >0 时 R 被夹到 ≤ 1−它 ⇒ 灰区也会漏出天空。只为"看看强制透射会怎样"，不是物理量。
    float min_transmission = 0.0f;

    // ── 波浪：**只扰法线，不动几何** ──
    // 相位（弧度）：调用方自己推进时钟（引擎不取自己的时间，"调相位"= 改它）。
    float wave_phase = 0.0f;
    // 最大斜率（度）——"波幅"的真实物理含义。0 = 完全平静（法线恒为 +Y）。
    float wave_slope_deg = 10.0f;
    // 主波长（米）与传播方向（世界 XZ 平面，度）。
    float wave_length_m = 0.5f;
    float wave_dir_deg = 30.0f;

    // ── 水上视角的三项（默认全开；关掉任一项就是 A/B 用）──
    bool use_sky_reflection = true;   // 镜面天光（天空探针按反射方向采样）
    bool use_transmission = true;     // 水下透射（关 ⇒ 水面变成不透明的本体色）
    bool use_sun_glint = true;        // 太阳高光（Blinn-Phong 单瓣 × F）
    float sky_scale = 1.0f;           // 天光反射量 [0,1]
    float sun_shininess = 1500.0f;    // 高光幂次（越大越尖）
    // 远距离波浪带限方式（对比/教学用，见 water_skin_shader.h 的 WaveNormal）：
    //   0 = 不过滤（aliasing 基线） 1 = 硬关八度+归一化 2 = 逐八度 sinc（IQ）
    //   3 = sinc + 方差转粗糙度    4 = **法线 flipbook 纹理 + 硬件 mip**
    int wave_filter_mode = 3;
    // 模式 4 的瓦片边长（米）：一张 tile 覆盖多大世界范围。
    float wave_tile_m = 16.0f;
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
    //   sky_env_cube      : 天空探针 cubemap（镜面天光用）。0 = 没有探针 ⇒ 退回常量 rim 色。
    //   sun               : 平行光（太阳/月亮）；nullopt = 不画太阳高光。
    //   width / height    : 3D FBO 尺寸（把 gl_FragCoord 换成 [0,1] uv）。
    // Pre-condition: params.half_extent_m > 0；scene_depth_tex != 0。
    // 返回后 GL_BLEND / GL_DEPTH_TEST / GL_CULL_FACE 均为关（与 3D pass 结束后的状态一致）。
    void Draw(const WaterSkinParams& params, const Camera& cam, const float view_proj[16],
              unsigned int scene_depth_tex, unsigned int sky_env_cube,
              const DirectionalLight* sun, int width, int height,
              ShaderManager& shader_mgr);

private:
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    // 波浪法线 flipbook 图集（Init 时烘一次；模式 4 用，其它模式也建好备用 —— 建一次
    // 约 1~2 s/256²×8 帧，留在 Init 里不占每帧成本）。
    WaterNormalTile wave_tile_;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

// JPOV WaterSkinRenderer —— 无限水平平面的「水皮」层（只产层，不合成）
//
// 定位（契约 docs/jpov_water_contract.md §2）：
//   水皮只画**界面**这一层：菲涅尔反射 + 透射（水上）/ Snell 窗 + 深海积分反射（水下）。
//   界面的**合成**由合成器负责（它按「场段 ↔ 层」交替积分），本类只产出一层 WBO 覆盖集：
//     accum = (C, α)、reveal = α、att3 = t·α（**射线距离**）。
//
// 为什么不在本类里 composite（Danis 2026-11-16 定）：
//   合成顺序只能有一个出处。水皮自己 over 会把「相机 → 水面」那段水体的注入整段盖掉
//   （环外 α=1 时尤其明显）。层交给合成器后，那一段由场的差分自动补上。
//
// 画法：全屏三角形 + FS 里解析求交（见 water_skin_shader.h 顶部为什么不用网格）。
// 不采样场景颜色（背景由合成器的 dst 提供），也不需要抓帧副本。

#ifndef JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_
#define JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/lighting/ggx_specular.h"   // 天体角半径默认值（唯一出处）
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/transparent_layer.h"
#include "tools/jpov/src/water/water_medium_math.h"
#include "tools/jpov/src/water/water_skin_math.h"
#include "tools/jpov/src/water/water_wave_math.h"
#include "tools/jpov/src/wbo/wbo_target.h"

namespace jpov {

class WaterSkinRenderer {
public:
    // 一次水面 pass 的全部输入（POD，值语义，便于 GPU 测试直接构造）。
    struct Inputs {
        Camera camera;                  // 位置/朝向/近远/fov
        int full_w = 0;                 // 3D FBO 宽（= 水面 pass 的目标分辨率）
        int full_h = 0;
        unsigned int scene_depth_tex = 0;   // MRT#1 场景深度（R32F，1.0 = 背景）
        unsigned int env_cube_tex = 0;      // 天光探针（SkyOutput::kEnvProbe 渲的 cubemap）
        float env_scale = 1.0f;             // 天光反射消费量（= EnvSpecular::sky_scale）
        float level_y = 0.0f;               // 水面世界高度（米）
        float ior = water::kWaterIorDefault;  // 水的折射率（> 0）
        // 水体介质（水下支路的「深海积分反射」要用它；nullptr = 无介质 ⇒ S_P(∞) = 0）。
        const water::WaterMediumSample* medium = nullptr;

        // ── 水面波浪（法线扰动 / 高度置换）──
        // nullptr / !enabled / num_waves = 0 ⇒ 纯平面（与旧版逐字节一致）。
        // 波浪只改界面几何（求交与法线）；水体介质仍按 level_y 的半空间积分。
        const WaterWaves* waves = nullptr;
        // flow map 的 GL 2D 纹理（RG 编码 (flow*0.5+0.5)；0 = 无 flow）。
        // 由 Renderer 从 WaterWaves::flow_texture_id 经 TextureManager 解析。
        unsigned int wave_flow_tex = 0;
        // debug：0 = 正常；1 = 把交点法线编码成颜色输出（定位法线噪点用）。
        int wave_debug = 0;

        // ── 太阳的像（解析 GGX，盘状光源）──
        // 与介质那份 sun 分开：介质可以关，水面依然该有日盘像。
        // has_sun = false ⇒ 这一项恒 0（不是“用强度 0 蹭过去”）。
        bool has_sun = false;
        Vec3f sun_toward = {0.0f, 1.0f, 0.0f};      // 指向太阳（气侧，单位）
        Vec3f sun_irradiance = {0.0f, 0.0f, 0.0f};  // color × intensity（辐照度语义）
        // 盘角半径（弧度，> 0）。默认太阳；场景覆盖。
        float sun_angular_radius = lighting::kSunAngularRadiusRad;
        // 像素足迹带宽开关：1 = 开（默认，掠射抗闪）；0 = 关（CPU 对拍测试用，
        // 因为着色器里的 dFdx 无法在 CPU 复现）。
        float sun_footprint = 1.0f;
    };

    WaterSkinRenderer();
    ~WaterSkinRenderer();

    WaterSkinRenderer(const WaterSkinRenderer&) = delete;
    WaterSkinRenderer& operator=(const WaterSkinRenderer&) = delete;

    // Pre-condition: shader_mgr != nullptr；GL context 已激活。
    void Init(ShaderManager* shader_mgr);

    // 释放 WBO 附件 / VAO。可重复调用（须在 GL context 存活时）。
    void Finalize();

    // 建/清本帧的 WBO 目标（尺寸变化会重建）。不画东西。
    // Pre-condition: 已 Init；full_w/full_h > 0。
    void Begin(int full_w, int full_h);

    // 画水面到 WBO 目标（只写层，不碰 HDR 颜色）。
    // Pre-condition: Begin 已调用；inputs.scene_depth_tex 非 0；camera.fbo_3d_* 与 full_w/h 一致。
    void Draw(const Inputs& inputs);

    // 本帧这一层的三张附件（交给合成器的 TransparentLayerSet）。
    // 未 Begin / 没画过内容时返回的层 invalid()。
    TransparentLayer layer() const;

private:
    void EnsureTarget(int w, int h);
    void UploadCommonUniforms(unsigned int prog, const Inputs& inputs);
    // 波浪谱 / flow map / 淡出等 uniform（uWave*）。
    void UploadWaveUniforms(unsigned int prog, const Inputs& inputs);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int vao_ = 0;
    WboTarget wbo_;
    bool has_content_ = false;
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

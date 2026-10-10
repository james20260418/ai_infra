// JPOV Fire-Fog froxel —— gold test 共享场景（generator/test 共用，防分叉）
//
// 目的：给 froxel 体积雾（fire_fog）建**确定性**的肉眼+回归基准。两种场景：
//   A. god ray  （FireFogGoldApp）：单个点雾 + 太阳×CSM 阴影（god ray），
//      相机对着树根斜 45° 俯视、方位面冲太阳（θ = sun_azim + 180）。
//   B. 点光源   （FireFogPointLightGoldApp）：单个点雾 + **一盏点光源**驱动内散射
//      （验证 fire_fog 自持的点光源 tile culling / 点光内散射链路），
//      点光偏置在雾内一侧 ⇒ 雾呈**不对称**暖色散射。
//
// 两场景共用一套「地面 + 高模橡树 + 相机 + 天光」（基类 FireFogGoldScene）；
// 子类只负责「雾 + 光照」。generate（写仓库 gold png）与 test（渲染 + 比对）**共用本文件**，
// 保证两边逐参数一致，避免各写一套而分叉。
//
// ⚠️ 橡树是 assets 里的「负样本」（树冠为离散面片），**不是**拿来做模型质量基准——
//    只当**遮挡物/布景**用。gold 的回归对象是「雾 + 光照」这条渲染链路。

#ifndef JPOV_TEST_FIRE_FOG_FIRE_FOG_GOLD_COMMON_H_
#define JPOV_TEST_FIRE_FOG_FIRE_FOG_GOLD_COMMON_H_

#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"   // 复用 MakeGroundQuad / MakeSkyLighting / DirFromAngles
#include "tools/jpov/test/test_utils.h"       // GetModelsDir / GetTestDataDir
#include "tools/common/utils.h"

namespace jpov_fire_fog_gold {

// ── 渲染分辨率 ──
inline constexpr int kResW = 1280;
inline constexpr int kResH = 720;

// ── 场景常量（gold 的「参数」，改了必须重出 gold）──
inline constexpr float kOakTargetHeight = 6.0f;   // 橡树归一世界高度（米）

// 光照：低太阳（长影，god ray 明显），方位 0（+Z）；turbidity 2（清澈）。
inline constexpr float kSunElevDeg = 20.0f;
inline constexpr float kSunAzimDeg = 0.0f;
inline constexpr float kTurbidity   = 2.0f;

// 相机：目标 = 树根（原点）；仰角 +30°（俯视 30°）；R 到原点（米）；
// 方位 = sun_azim + 180 ⇒ 视线水平朝 **太阳方位**（面冲太阳）。
inline constexpr float kCamPhiDeg  = 30.0f;
inline constexpr float kCamR       = 6.0f;
inline constexpr float kViewFovDeg = 60.0f;
inline constexpr float kViewNear   = 0.05f;
inline constexpr float kViewFar    = 1000.0f;

// ── 场景 A（god ray）：点雾 kLinear 剖面、σ=1.0、albedo (0.25,0.25,0.25)、无自发光。──
// （旧模型 L_in = color·光照；新模型散射 = albedo·光照 ⇒ albedo=旧 color、emission=0 时逐像素等价。）
inline constexpr float kFogCenter[3] = {0.0f, 3.0f, 0.0f};
inline constexpr float kFogRadius    = 4.5f;
inline constexpr float kFogSigma     = 1.0f;
inline constexpr float kFogAlbedo    = 0.25f;

// 雾火光照（场景 A/B 共用）：太阳项相位/增益。
inline constexpr float kSunPhaseG = 0.7f;
inline constexpr float kSunGain   = 2.0f;

// ── 场景 B（点光源驱动雾散射）：点雾 + 一盏暖色点光偏置在雾内一侧。──
inline constexpr float kPlFogCenter[3] = {0.0f, 3.0f, 0.0f};
inline constexpr float kPlFogRadius    = 4.5f;
inline constexpr float kPlFogSigma     = 1.0f;
inline constexpr float kPlFogAlbedo    = 0.25f;              // 与 god ray gold 同（唯一差异 = 点光源）
inline constexpr float kPlPos[3]       = {2.5f, 3.0f, 1.5f};   // 点光位置（雾内偏一侧 ⇒ 不对称散射）
inline constexpr float kPlColor[3]     = {1.0f, 0.55f, 0.25f};  // 暖色（火光感）
inline constexpr float kPlRadius       = 7.0f;                 // 线性衰减半径（米）——收紧使光局部
inline constexpr float kPlIntensity    = 22.0f;                // 亮度标量

// 橡树模型相对 assets/models 的路径。
inline const char* kOakRelPath = "samples/oak_negative/tripo_oak_4k.glb";

// 橡树模型绝对路径（runfiles / project root 自适应）。
inline std::string OakPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        return p + "__main__/tools/jpov/assets/models/" + kOakRelPath;
    }
    return jpov::GetModelsDir() + "/" + kOakRelPath;
}

// ── 共享场景基类：地面 + 橡树 + 相机 + 天光；子类只实现 EmitFogAndLighting。──
class FireFogGoldScene : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    jpov::GltfObject oak_;
    float oak_scale_ = 1.0f;

    // 装配场景静态资源（Init 之后调一次）。
    void Install() {
        ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
        ground_mat_ = jpov_skylight::GroundMaterial();

        oak_ = LoadGltf(OakPath());
        CHECK(!oak_.empty()) << "LoadGltf failed: " << OakPath();
        float scale = 1.0f;
        if (oak_.bounds_valid) {
            const float h = oak_.bounds_max[1] - oak_.bounds_min[1];
            CHECK_GT(h, 1.0e-6f) << "橡树模型 Y 向高度≈0";
            scale = kOakTargetHeight / h;
        } else {
            LOG(WARNING) << "橡树模型无 bounds，scale 置 1";
        }
        oak_scale_ = scale;
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_  = static_cast<float>(kResW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kResH);

        // 相机：树根（原点）方向；方位默认面冲太阳。
        const jpov::Vec3f cam_dir =
            jpov_skylight::DirFromAngles(CameraPhiDeg(), CameraAzimDeg());
        cmds->camera.position = {kCamR * cam_dir.x(), kCamR * cam_dir.y(),
                                 kCamR * cam_dir.z()};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = kViewFovDeg;
        cmds->camera.near     = kViewNear;
        cmds->camera.far      = kViewFar;

        // 光照：白昼标准天光（单色 ambient）。
        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(SkyDegreesForGold(), /*tricolor_ambient=*/false);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;   // fire_fog pass 要求线性 HDR 域

        // 场景：灰地面 + 橡树。
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(oak_, /*center*/ {0.0f, 0.0f, 0.0f},
                             /*up*/ {0.0f, 1.0f, 0.0f},
                             /*front*/ {0.0f, 0.0f, 1.0f}, oak_scale_);

        EmitFogAndLighting(cmds);
    }

protected:
    virtual float CameraPhiDeg() const { return kCamPhiDeg; }
    virtual float CameraAzimDeg() const { return kSunAzimDeg + 180.0f; }

    virtual jpov_skylight::SkyDegrees SkyDegreesForGold() const {
        jpov_skylight::SkyDegrees d;
        d.sun_elev_deg = kSunElevDeg;
        d.sun_azim_deg = kSunAzimDeg;
        d.turbidity    = kTurbidity;
        return d;
    }

    // 子类：填点雾 + 点光源 + FireFogParams。
    virtual void EmitFogAndLighting(jpov::RenderCommandList* cmds) = 0;
};

// 场景 A：god ray（单点雾 + 太阳×CSM，无点光源）。
class FireFogGoldApp : public FireFogGoldScene {
public:
    using FireFogGoldScene::FireFogGoldScene;

protected:
    void EmitFogAndLighting(jpov::RenderCommandList* cmds) override {
        jpov::PointFog fog;
        fog.center = {kFogCenter[0], kFogCenter[1], kFogCenter[2]};
        fog.radius = kFogRadius;
        fog.sigma = kFogSigma;
        fog.albedo = {kFogAlbedo, kFogAlbedo, kFogAlbedo, 1.0f};
        fog.emission = {0.0f, 0.0f, 0.0f, 1.0f};
        fog.attenuation = jpov::FogAttenuation::kLinear;
        cmds->point_fogs.push_back(fog);

        jpov::FireFogParams ff;
        ff.sun_phase_g = kSunPhaseG;
        ff.sun_gain = kSunGain;
        cmds->fire_fog = ff;
    }
};

// 场景 B：点光源驱动雾散射（单点雾 + 一盏暖色点光）。
class FireFogPointLightGoldApp : public FireFogGoldScene {
public:
    using FireFogGoldScene::FireFogGoldScene;

protected:
    void EmitFogAndLighting(jpov::RenderCommandList* cmds) override {
        jpov::PointFog fog;
        fog.center = {kPlFogCenter[0], kPlFogCenter[1], kPlFogCenter[2]};
        fog.radius = kPlFogRadius;
        fog.sigma = kPlFogSigma;
        fog.albedo = {kPlFogAlbedo, kPlFogAlbedo, kPlFogAlbedo, 1.0f};
        fog.emission = {0.0f, 0.0f, 0.0f, 1.0f};
        fog.attenuation = jpov::FogAttenuation::kLinear;
        cmds->point_fogs.push_back(fog);

        jpov::PointLight pl;
        pl.position = {kPlPos[0], kPlPos[1], kPlPos[2]};
        pl.color = {kPlColor[0], kPlColor[1], kPlColor[2], 1.0f};
        pl.linear_radius = kPlRadius;
        pl.intensity = kPlIntensity;
        cmds->point_lights.push_back(pl);

        jpov::FireFogParams ff;
        ff.sun_phase_g = kSunPhaseG;
        ff.sun_gain = kSunGain;
        cmds->fire_fog = ff;
    }
};

// gold png 文件名（仓库内，供 generator 写 / test 读）。
inline const char* kGoldPngName = "fire_fog_godray_1280x720.png";
inline const char* kPointLightGoldPngName = "fire_fog_pointlight_1280x720.png";

}  // namespace jpov_fire_fog_gold

#endif  // JPOV_TEST_FIRE_FOG_FIRE_FOG_GOLD_COMMON_H_

// JPOV Fire-Fog froxel —— gold test 共享场景 + 渲染体（generator/test 共用，防分叉）
//
// 目的：给 froxel 体积雾（fire_fog）建一个**确定性**的肉眼+回归基准：
//   场景 = 灰地面 + 高模橡树（kOakTargetHeight 归一）；**开启太阳光照**（CSM god ray）；
//   单个点雾：kLinear 剖面、σ = 1.0、色 (0.25,0.25,0.25)；
//   相机：**对着树根（原点）斜 45° 俯视**，方位 **xy 方向面冲太阳**（θ = sun_azim + 180）。
//   froxel 用默认网格（Nz=256 / tile=16 / far=2000）。
//
// generator（写仓库 gold png）与 test（渲染 + 比对）**共用本文件的 App 与常量**，
// 保证两边逐参数一致，避免各写一套而分叉。
//
// ⚠️ 橡树是 assets 里的「负样本」（树冠为离散面片），**不是**拿来做模型质量基准——
//    这里只把它当**遮挡物/布景**用（与 fire_fog 查看器同款场景），gold 的回归对象是
//    「雾 + CSM god ray」这条渲染链路。

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

// 相机：目标 = 树根（原点）；仰角 +45°（俯视 45°）；R 到原点 12m；
// 方位 = sun_azim + 180 ⇒ 视线水平朝 **太阳方位**（面冲太阳）。
inline constexpr float kCamPhiDeg  = 45.0f;
inline constexpr float kCamR       = 12.0f;
inline constexpr float kViewFovDeg = 60.0f;
inline constexpr float kViewNear   = 0.05f;
inline constexpr float kViewFar    = 1000.0f;

// 点雾：kLinear 剖面、σ=1.0、色 (0.25,0.25,0.25)，摆在中段（半径 3m；实测该构图
// 雾成整体、树仍可读、god ray 最明显——r=5 会把树糊成一片、r=2 太像局部光晕）。
inline constexpr float kFogCenter[3] = {0.0f, 3.0f, 0.0f};
inline constexpr float kFogRadius    = 3.0f;
inline constexpr float kFogIntensity = 1.0f;
inline constexpr float kFogColor     = 0.25f;

// 雾火光照（step2）：太阳项相位/增益。
inline constexpr float kSunPhaseG = 0.7f;
inline constexpr float kSunGain   = 2.0f;

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

// 确定性 gold 渲染体：装配一次场景，每帧照固定参数发命令。
class FireFogGoldApp : public JPOV {
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

        // 相机：树根（原点）方向；方位面冲太阳。
        const jpov::Vec3f cam_dir =
            jpov_skylight::DirFromAngles(kCamPhiDeg, kSunAzimDeg + 180.0f);
        cmds->camera.position = {kCamR * cam_dir.x(), kCamR * cam_dir.y(),
                                 kCamR * cam_dir.z()};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = kViewFovDeg;
        cmds->camera.near     = kViewNear;
        cmds->camera.far      = kViewFar;

        // 光照：白昼标准天光（单色 ambient），与查看器同源。
        jpov_skylight::SkyDegrees deg;
        deg.sun_elev_deg = kSunElevDeg;
        deg.sun_azim_deg = kSunAzimDeg;
        deg.turbidity    = kTurbidity;
        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg, /*tricolor_ambient=*/false);
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

        // 点雾（kLinear / σ=1.0 / 色 0.25）——走 froxel 体积管线。
        jpov::PointFog fog;
        fog.center = {kFogCenter[0], kFogCenter[1], kFogCenter[2]};
        fog.radius = kFogRadius;
        fog.color = {kFogColor, kFogColor, kFogColor, 1.0f};
        fog.intensity = kFogIntensity;
        fog.attenuation = jpov::FogAttenuation::kLinear;
        cmds->point_fogs.push_back(fog);

        // 雾火参数：开启太阳（CSM god ray）；nz/tile/z_far 走默认（256/16/2000）。
        jpov::FireFogParams ff;
        ff.sun_enable = true;
        ff.sun_phase_g = kSunPhaseG;
        ff.sun_gain = kSunGain;
        cmds->fire_fog = ff;
    }
};

// gold png 文件名（仓库内，供 generator 写 / test 读）。
inline const char* kGoldPngName = "fire_fog_godray_1280x720.png";

}  // namespace jpov_fire_fog_gold

#endif  // JPOV_TEST_FIRE_FOG_FIRE_FOG_GOLD_COMMON_H_

// JPOV 阴影 PCF 采样核 gold test —— 共享场景（黄金角螺旋 vs 固定 3×3）
//
// 目的：给「阴影 PCF 采样核」上 gold 保护。场景/机位复刻天光查看器那张 PCF
// 对比图：灰地面 + 三方块（低反/高光/金属，各顶面站一个 T-pose 蓝人）+ 桌子 +
// 高模橡树；天光由 SkyCommand 推导（太阳仰角 25° / 方位角 45°），相机
// phi=0.35 / theta=45° / R=9.5。
//
// 该机位太阳低斜，方块/橡树/人在灰地面上投出大量横跨级联的**影子边缘** —— 是观察
// PCF 软化（黄金角螺旋 N 点 vs 3×3 固定核）的最佳画布：
//   - 固定 3×3 核：软宽仅 1 纹素，影缘呈明显阶梯；
//   - 黄金角螺旋 N=25/R=4：软宽 ~8 纹素，影缘平滑。
// gold 以 N=25/R=4 生成（软化态）；粗/细双闸门见 test。
//
// 本头文件被 generator 与 test 共用，保证两侧场景/光照/机位完全一致。

#ifndef JPOV_TEST_OBJECT3D_JPOV_PCF_SHADOW_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_PCF_SHADOW_COMMON_H_

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov_pcf_shadow {

// 仓库内 3D 资产根下的相对路径 → 可读路径（bazel test 沙箱走 TEST_SRCDIR）。
inline std::string AssetPath(const std::string& rel) {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir != nullptr) {
        std::string p(srcdir);
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/assets/models/" + rel;
    }
    return jpov::GetProjectRoot() + "tools/jpov/assets/models/" + rel;
}

// 对比用的机位 / 太阳角度（复刻 PCF 对比图）。generator 与 test 共用，保证一致。
inline constexpr float kViewPhi    = 0.35f;
inline constexpr float kViewTheta  = 3.14159265358979323846 / 4.0;  // 45°
inline constexpr float kViewR      = 9.5f;
inline constexpr float kSunElevDeg = 25.0f;
inline constexpr float kSunAzimDeg = 45.0f;

// 测试专用 App：固定机位/天光，并把指定 PCF 采样核每帧写入 cmds->shadow_pcf。
class PcfShadowApp : public jpov_skylight::SkylightApp {
public:
    using jpov_skylight::SkylightApp::SkylightApp;

    void SetShadowPcf(const jpov::ShadowPcfConfig& pcf) { pcf_ = pcf; }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        // 固定机位 + 天光自由度（不用默认值，保证 generator/test 逐位一致）。
        view_.phi   = kViewPhi;
        view_.theta = kViewTheta;
        view_.R     = kViewR;
        deg_.turbidity       = jpov_skylight::kTurbidityDef;
        deg_.daylight_season = 0.0f;
        deg_.sun_elev_deg    = kSunElevDeg;
        deg_.sun_azim_deg    = kSunAzimDeg;
        deg_.moon_red        = 0.0f;
        deg_.night_blue      = 0.0f;
        deg_.elev_fog_on     = false;
        cmds->shadow_pcf = pcf_;
        jpov_skylight::SkylightApp::OneIteration(frame_count, input, winfo, cmds);
    }

private:
    jpov::ShadowPcfConfig pcf_;
};

// 按目标世界高度求缩放（bounds 无效/高度≈0 时返回 1.0，不静默乱缩）。
inline float ScaleToHeight(const jpov::GltfObject& o, float target_h) {
    if (!o.bounds_valid) {
        LOG(WARNING) << "模型无 bounds，scale 置 1（不自动归一）";
        return 1.0f;
    }
    const float h = o.bounds_max[1] - o.bounds_min[1];
    if (h <= 1.0e-6f) {
        LOG(WARNING) << "模型 Y 向高度≈0，scale 置 1（不自动归一）";
        return 1.0f;
    }
    return target_h / h;
}

// 装载场景静态资源（复刻天光查看器 InstallScene/InstallModels/InstallPerson）：
//   灰地面 + 三方块 + 桌子（左，~0.75m）+ 高模橡树（右，~6m）+ 三个 T-pose 蓝人。
inline void AssembleScene(PcfShadowApp& app) {
    app.SetShowPanel(false);   // 纯 3D（无 UI 面板）

    app.box_mesh_    = app.RegisterMesh(jpov::MeshData::MakeBox(
        jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf));
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.mat_low_    = jpov_skylight::MaterialLowGloss();
    app.mat_high_   = jpov_skylight::MaterialHighGloss();
    app.mat_metal_  = jpov_skylight::MaterialMetal();
    app.mat_ground_ = jpov_skylight::GroundMaterial();

    // 桌子（低矮家具）：左侧，目标高 ~0.75 m。
    {
        jpov::GltfObject table =
            app.LoadGltf(AssetPath("scene/scene_assets/table.glb"));
        CHECK(!table.empty()) << "LoadGltf failed: table.glb";
        app.AddModel(std::move(table), /*center*/ {-2.6f, 0.0f, -0.4f},
                     /*up*/ {0, 1, 0}, /*front*/ {1, 0, 0},
                     /*scale*/ ScaleToHeight(table, 0.75f));
    }
    // 高模橡树：右侧，目标高 ~6 m。
    {
        jpov::GltfObject oak =
            app.LoadGltf(AssetPath("samples/oak_negative/tripo_oak_4k.glb"));
        CHECK(!oak.empty()) << "LoadGltf failed: tripo_oak_4k.glb";
        app.AddModel(std::move(oak), /*center*/ {3.2f, 0.0f, -0.6f},
                     /*up*/ {0, 1, 0}, /*front*/ {-1, 0, 0},
                     /*scale*/ ScaleToHeight(oak, 6.0f));
    }
    // 蓝人（mixamo_male）：一份蒙皮 mesh + 一份骨架，每个方块顶面一个 T-pose 实例。
    {
        const std::string male = AssetPath("characters/mixamo_male.glb");
        jpov::GltfObject obj = app.LoadGltf(male);
        CHECK(!obj.empty() && !obj.primitives.empty())
            << "LoadGltf failed: mixamo_male.glb";
        CHECK(obj.bounds_valid) << "蓝人资产无包围盒: " << male;
        const float asset_h = obj.bounds_max[1] - obj.bounds_min[1];
        CHECK_GT(asset_h, 1.0e-6f) << "蓝人资产 Y 向高度≈0";
        const float scale = jpov_skylight::kPersonTargetHeight / asset_h;
        const float min_y = obj.bounds_min[1];
        app.person_mesh_     = obj.primitives[0].mesh_id;
        app.person_material_ = obj.primitives[0].material;
        CHECK_NE(app.person_mesh_, 0u) << "蓝人网格句柄为 0";

        std::vector<jpov::SkeletonType> skins;
        CHECK(jpov::LoadGltfSkeleton(male, &skins) && !skins.empty())
            << "LoadGltfSkeleton 失败或无 skin: " << male;
        jpov::SkeletonType type = skins[0];
        type.Validate();
        app.person_skeleton_id_ = app.RegisterSkeleton(
            type, {jpov::SkeletonPose::Identity(type.bone_count())});

        app.person_instances_.clear();
        app.person_instances_.reserve(3);
        for (int i = 0; i < 3; ++i) {
            const jpov::Vec3f top = jpov_skylight::BoxTopCenter(i);
            jpov::SkinnedInstanceState inst;
            inst.transform.center = {top.x(), top.y() - scale * min_y, top.z()};
            inst.transform.up     = {0.0f, 1.0f, 0.0f};
            inst.transform.front  = {0.0f, 0.0f, 1.0f};
            inst.transform.scale  = scale;
            inst.pose_a = 0;   // 唯一一帧 = identity = T-pose
            inst.pose_b = 0;
            inst.ratio  = 0.0f;
            app.person_instances_.push_back(inst);
        }
        app.person_gltf_ = std::move(obj);   // 资源保活
    }
}

}  // namespace jpov_pcf_shadow

#endif  // JPOV_TEST_OBJECT3D_JPOV_PCF_SHADOW_COMMON_H_

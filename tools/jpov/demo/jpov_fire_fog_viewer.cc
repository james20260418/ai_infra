// JPOV 雾火查看器（fire_fog viewer）— 主程序
//
// 以 skylight viewer 为基础，**专门调试雾火（fire_fog）**：
//   - 场景精简：方块 3→1（中间一个）、蓝人 3→1（中间方块顶面一个）；地面/桌子/橡树同 skylight；
//   - 屏幕中心放一团 2 米点雾（fire_fog 管线输入）；
//   - 调试开关：把「有雾火元素覆盖的屏幕 tile」涂一层 color blend，肉眼核对 L1 tile 剪枝。
//
// 架构与 skylight viewer 同构：主程序只做装配 + 事件循环；场景/天光装配在
// skylight_scene.h（复用），渲染核心 + 面板在 fire_fog_viewer_app.h。
//
// 编译运行：
//   ./tools/jpov/build_jpov_fire_fog_viewer.sh
//   → output/jpov_fire_fog_viewer/jpov_fire_fog_viewer
// headless 拍摄（出图验收）：
//   output/jpov_fire_fog_viewer/jpov_fire_fog_viewer --capture <out_dir>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/fire_fog_viewer_app.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/common/utils.h"

namespace {

// 装配场景静态资源（交互与 headless 共用）。只保留中间一个方块（skylight 的第 1 号）。
void InstallScene(jpov_fire_fog::FireFogApp& app) {
    app.box_mesh_    = app.RegisterMesh(jpov::MeshData::MakeBox(
        jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf,
        jpov_skylight::kBoxHalf));
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.box_material_ = jpov_skylight::MaterialHighGloss();   // 中间方块原材质
    app.ground_material_ = jpov_skylight::GroundMaterial();
}

// 额外模型（桌子 / 高模橡树）的相对路径（相对 exe 所在目录）。
inline constexpr const char* kTableModelPath = "models/table.glb";
inline constexpr const char* kOakModelPath   = "models/tripo_oak_4k.glb";
inline constexpr const char* kMaleModelPath  = "models/mixamo_male.glb";

// 装载额外模型（同 skylight viewer：桌子放左、橡树放右，按目标高度归一）。
void InstallModels(jpov_fire_fog::FireFogApp& app) {
    auto load = [&app](const char* rel) {
        jpov::GltfObject o = app.LoadGltf(rel);
        CHECK(!o.empty()) << "LoadGltf failed: " << rel;
        return o;
    };
    auto scale_to_height = [](const jpov::GltfObject& o, float target_h) {
        if (!o.bounds_valid) {
            return 1.0f;
        }
        const float h = o.bounds_max[1] - o.bounds_min[1];
        if (h <= 1.0e-6f) {
            return 1.0f;
        }
        return target_h / h;
    };
    jpov::GltfObject table = load(kTableModelPath);
    const float table_scale = scale_to_height(table, 0.75f);
    app.AddModel(std::move(table), /*center*/ {-2.6f, 0.0f, -0.4f},
                 /*up*/ {0, 1, 0}, /*front*/ {1, 0, 0}, table_scale);
    jpov::GltfObject oak = load(kOakModelPath);
    const float oak_scale = scale_to_height(oak, 6.0f);
    app.AddModel(std::move(oak), /*center*/ {3.2f, 0.0f, -0.6f},
                 /*up*/ {0, 1, 0}, /*front*/ {-1, 0, 0}, oak_scale);
}

// 蓝人（mixamo_male）：中间方块顶面摆一个 T-pose 实例（同 skylight viewer 的装配，只留 1 个）。
void InstallPerson(jpov_fire_fog::FireFogApp& app) {
    jpov::GltfObject obj = app.LoadGltf(kMaleModelPath);
    CHECK(!obj.empty()) << "LoadGltf failed: " << kMaleModelPath;
    CHECK(!obj.primitives.empty());
    CHECK(obj.bounds_valid) << "蓝人资产无包围盒: " << kMaleModelPath;
    const float asset_h = obj.bounds_max[1] - obj.bounds_min[1];
    CHECK_GT(asset_h, 1.0e-6f);
    const float scale = jpov_skylight::kPersonTargetHeight / asset_h;
    const float min_y = obj.bounds_min[1];

    app.person_mesh_     = obj.primitives[0].mesh_id;
    app.person_material_ = obj.primitives[0].material;
    CHECK_NE(app.person_mesh_, 0u);

    const std::string skel_path = jpov::ResolveResourcePath(kMaleModelPath);
    std::vector<jpov::SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(skel_path, &skins) && !skins.empty())
        << "LoadGltfSkeleton 失败或无 skin: " << skel_path;
    jpov::SkeletonType type = skins[0];
    type.Validate();
    app.person_skeleton_id_ = app.RegisterSkeleton(
        type, {jpov::SkeletonPose::Identity(type.bone_count())});

    // 只留中间方块顶面一个实例。
    const jpov::Vec3f top = jpov_skylight::BoxTopCenter(1);
    jpov::SkinnedInstanceState inst;
    inst.transform.center = {top.x(), top.y() - scale * min_y, top.z()};
    inst.transform.up     = {0.0f, 1.0f, 0.0f};
    inst.transform.front  = {0.0f, 0.0f, 1.0f};
    inst.transform.scale  = scale;
    inst.pose_a = 0;
    inst.pose_b = 0;
    inst.ratio  = 0.0f;
    app.person_instances_.clear();
    app.person_instances_.push_back(inst);
    app.person_gltf_ = std::move(obj);
}

// headless 拍摄：出两张图 —— ① tile 调试（有元素的 tile 涂色）；② 关调试（真实场景）。
int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 雾火查看器（headless capture）";
    cfg.width  = jpov_fire_fog::kViewerWidth;
    cfg.height = jpov_fire_fog::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_fire_fog::FireFogApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_fire_fog::kViewerWidth;
    winfo.height = jpov_fire_fog::kViewerHeight;
    jpov::InputSnapshot input{};

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.0;
    app.deg_.elev_fog_on = false;   // 关远景雾，专注看局部点雾

    // ① tile 调试：屏幕中心 2 米点雾 → 覆盖的 tile 应被涂成绿色块。
    app.fog_on_ = true;
    app.fog_radius_ = 2.0f;
    app.fog_center_y_ = 1.0f;
    app.fog_debug_tiles_ = true;
    {
        const std::string path = out_dir + "/tile_debug.png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    }

    // ② 关 tile 调试（对照：应为无涂色的真实场景）。
    app.fog_debug_tiles_ = false;
    {
        const std::string path = out_dir + "/no_debug.png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    }

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir);
    }

    // ── 交互模式 ──
    JPOV::Config cfg;
    cfg.title  = "JPOV — 雾火查看器（fire_fog viewer）";
    cfg.width  = jpov_fire_fog::kViewerWidth;
    cfg.height = jpov_fire_fog::kViewerHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_fire_fog::kViewerFps);
    cfg.headless   = false;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_fire_fog::FireFogApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();

    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.0;
    app.deg_.elev_fog_on = false;

    app.Run();

    app.Finalize();
    return 0;
}

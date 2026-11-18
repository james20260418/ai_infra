// JPOV Gold Image Generator — 点光源的镜面带宽（逐像素角半径 θ）
//
// 写仓库 gold PNG（tools/jpov/test/object3d/）：
//   point_specular_r0.000_1280x720.png              等效 δ（修之前：地上没有灯的像）
//   point_specular_r0.020_1280x720.png              小球灯
//   point_specular_r0.100_1280x720.png              中等球灯
//   point_specular_r0.500_1280x720.png              大球灯（像最大最柔）
//   point_specular_r0.100_rough0.300_1280x720.png   粗糙端（roughness 主导）
//   point_specular_r0.100_mirror_1280x720.png       开天光反射的镜子观感
//
// 场景/机位/灯位的唯一出处：demo/skylight_viewer_app.h 的
//   InstallMirrorFloorScene() + SetPointSpecularView() + PlacePointLampAtScreenCenter()；
// 每张只差的条件在同目录 jpov_point_specular_gold_common.h 的 kShots（test 复用同一份）。
//
// 用法：bazel run //tools/jpov/test/object3d:jpov_point_specular_gold_generator

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_point_specular_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string out_dir = jpov::GetTestDataDir() + "/object3d/";

    JPOV::Config cfg;
    cfg.title = "Point Specular Bandwidth (sphere light radius) Generator";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    jpov_point_specular::InstallScene(&app);

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(jpov_point_specular::kImageWidth);
    winfo.height = static_cast<float>(jpov_point_specular::kImageHeight);
    jpov::InputSnapshot input{};

    for (int i = 0; i < jpov_point_specular::kShotCount; ++i) {
        const jpov_point_specular::Shot& shot = jpov_point_specular::kShots[i];
        jpov_point_specular::ApplyShot(&app, shot);
        const std::string path = out_dir + jpov_point_specular::ShotFileName(shot);
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << shot.file_tag << " → " << path;
    }

    app.ClearPointLamp();
    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    return 0;
}

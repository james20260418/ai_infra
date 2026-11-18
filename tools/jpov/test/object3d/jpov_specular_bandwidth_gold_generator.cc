// JPOV Gold Image Generator — 镜面高光的带宽（光源角半径 θ）
//
// 渲染 kShots 里的每一组对照条件，写到 tools/jpov/test/object3d/：
//   specular_bw_r0.000_pure_1280x720.png                只有太阳的镜面项（钱图）
//   specular_bw_r0.000_pure_no_bandwidth_1280x720.png   θ=1e-6（等效 δ）负向对照
//   specular_bw_r0.000_mirror_1280x720.png              镜子观感（映天光与方块）
//   specular_bw_r0.000_mirror_no_bandwidth_1280x720.png 同上，θ=1e-6
//   specular_bw_r0.020_pure_1280x720.png                roughness=0.02（θ 主导）
//   specular_bw_r0.300_mirror_1280x720.png              roughness 主导的对照
//
// 场景/机位/光照的唯一出处是 demo/skylight_viewer_app.h 的
// InstallMirrorFloorScene() + SetMirrorSpecularView()；每张只差的条件在
// 同目录 jpov_specular_bandwidth_gold_common.h 的 kShots 里（test 复用同一份）。
//
// 用法：bazel run //tools/jpov/test/object3d:jpov_specular_bandwidth_gold_generator

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_specular_bandwidth_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string out_dir = jpov::GetTestDataDir() + "/object3d/";

    JPOV::Config cfg;
    cfg.title = "Specular Bandwidth (sun angular radius) Generator";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallMirrorFloorScene();
    app.SetMirrorSpecularView();

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(jpov_specular_bandwidth::kImageWidth);
    winfo.height = static_cast<float>(jpov_specular_bandwidth::kImageHeight);
    jpov::InputSnapshot input{};

    for (int i = 0; i < jpov_specular_bandwidth::kShotCount; ++i) {
        const jpov_specular_bandwidth::Shot& shot =
            jpov_specular_bandwidth::kShots[i];
        jpov_specular_bandwidth::ApplyShot(&app, shot);
        const std::string path = out_dir +
            jpov_specular_bandwidth::ShotFileName(shot);
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << shot.file_tag << " → " << path;
    }

    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    return 0;
}

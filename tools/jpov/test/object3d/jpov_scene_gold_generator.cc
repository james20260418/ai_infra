// JPOV 场景 gold image generator —— 3 点光源版（旧 baseline）
//
// 渲染 3 点光源场景并写仓库内的 gold image（供 leader/Danis 肉眼查看效果）：
//   tools/jpov/test/object3d/scene_1280x720.png
//
// 与 jpov_sun_scene_gold_test（太阳+阴影版）共用 jpov_scene_common.h 的场景
// 几何，仅光照模式不同（默认 LightMode::kPointLights = 旧基线）。
// 输出: 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_scene_common.h"

int main() {
    const std::string outpath =
        "/james_pm/ai_infra/tools/jpov/test/object3d/scene_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Scene Gold Generator (3 point lights)";
    cfg.headless = true;
    jpov_scene::SceneApp app(cfg);
    app.Init();
    jpov_scene::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "场景 gold image generated: " << outpath;
    return 0;
}

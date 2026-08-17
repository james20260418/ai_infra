// JPOV 太阳场景 gold image generator —— 太阳平行光 + 阴影 + 天光版
//
// 渲染太阳场景并写仓库内的 gold image（供 leader/Danis 肉眼查看对比效果）：
//   tools/jpov/test/object3d/sun_scene_1280x720.png
//
// 与 jpov_scene_gold_test（旧 3 点光源 baseline）共用 jpov_scene_common.h 的
// 场景几何，仅光照模式不同（LightMode::kSun），便于与旧点光源版并排对比。
// 输出: 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_scene_common.h"

int main() {
    const std::string outpath =
        "/james_pm/ai_infra/tools/jpov/test/object3d/sun_scene_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Sun Scene Gold Generator (sun + shadow)";
    cfg.headless = true;
    jpov_scene::SceneApp app(cfg);
    app.Init();
    jpov_scene::BuildScene(&app);
    app.SetLightMode(jpov_scene::LightMode::kSun);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "太阳场景 gold image generated: " << outpath;
    return 0;
}

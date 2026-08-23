// JPOV 场景 gold image generator —— 阳光版
//
// 渲染阳光场景（石头墙 + 5×5 地面 + 桌/凳/盆栽，太阳平行光 direction=(0,-1,-1)
// + CSM 阴影）并写仓库内的 gold image（供 leader/Danis 肉眼查看效果）：
//   tools/jpov/test/object3d/scene_in_sun_1280x720.png
//
// 与 jpov_scene_in_sun_gold_test 共用 jpov_scene_in_sun_common.h 的场景构建。
// 与点光源版（scene_1280x720.png）唯一区别：光照改用太阳（去点光源）。
// 输出: 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_scene_in_sun_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/scene_in_sun_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Scene Gold Generator (sun)";
    cfg.headless = true;
    jpov_scene_in_sun::SceneSunApp app(cfg);
    app.Init();
    jpov_scene_in_sun::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "场景(阳光版) gold image generated: " << outpath;
    return 0;
}

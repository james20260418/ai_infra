// JPOV picking + highlight —— standard_sunny_day 基底 gold image generator
//
// 以 standard_sunny_day 场景为底座，对 stool（凳子）+ wall（墙）开启方法 B
// 高亮纯色边框，渲染高亮后的整体效果并写仓库 gold image：
//   tools/jpov/test/object3d/skydome/pick_highlight_sunny_1280x720.png
//
// 供肉眼参考高亮效果 + test 做 smoke/本体色断言。输出 640x360
//（文件名 1280x720 仅为分辨率标记，与 standard_sunny_day 惯例一致）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_pick_highlight_sunny_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/skydome/"
        "pick_highlight_sunny_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Pick+Highlight Sunny Gold Generator";
    cfg.headless = true;
    jpov_pick_highlight_sunny::PickHighlightSunnyApp app(cfg);
    app.Init();
    // 高亮 stool + wall（两者同时可拾取）。
    jpov_pick_highlight_sunny::BuildScene(&app,
                                          /*highlight_stool=*/true,
                                          /*highlight_wall=*/true);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "pick+highlight sunny gold image generated: " << outpath;
    return 0;
}

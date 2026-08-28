// JPOV 标准晴天（standard_sunny_day）gold image generator
//
// 渲染"正午晴天"场景（石头墙 + 5×5 地面 + 桌/凳/盆栽 + Preetham 天光 +
// 太阳平行光 + CSM 阴影）并写仓库 gold image：
//   tools/jpov/test/object3d/skydome/standard_sunny_day_1280x720.png
//
// 用于调试天空（DaySkyCommand）与太阳直射（DirectionalLight）在 HDR 链路上
// 的对齐。tone_mapping 开启、天空 intensity=1.0，供 Danis 手动调 sky.intensity。
// 输出 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_standard_sunny_day_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/skydome/"
        "standard_sunny_day_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Standard Sunny Day Gold Generator";
    cfg.headless = true;
    jpov_standard_sunny_day::StandardSunnyDayApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "standard sunny day gold image generated: " << outpath;
    return 0;
}

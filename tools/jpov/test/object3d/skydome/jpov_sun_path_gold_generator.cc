// JPOV 太阳轨迹（sun path）gold image generator
//
// 遍历 SkyDirectionAmbient 配置序列，为每个时段渲染一张天光图片，写仓库
// gold image：sun_path_<index>.png（index 从 0 起，后缀即配置在 vector 中的下标）。
//
// 相机 + 场景复用 standard_sunny_day（石头墙 + 5×5 地面 + 桌/凳/盆栽），
// 光照参数由 DefaultSunPath() 的 vector 驱动。当前 vector 尺寸 = 1（正午晴天），
// 后续往 DefaultSunPath 加配置即可生成太阳从早到晚的完整时段。
// 输出 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_sun_path_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV Sun Path Gold Generator";
    cfg.headless = true;
    jpov_sun_path::SunPathApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);

    std::vector<jpov_sun_path::SkyDirectionAmbient> path =
        jpov_sun_path::DefaultSunPath();
    app.configs_ = std::move(path);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    for (int i = 0; i < static_cast<int>(app.configs_.size()); ++i) {
        app.active_index_ = i;
        std::string outpath = jpov::GetTestDataDir()
            + "/object3d/skydome/sun_path_" + std::to_string(i) + ".png";
        app.RunOnce(input, winfo, outpath.c_str());
        LOG(INFO) << "sun path gold [" << i << "]: " << outpath;
    }

    app.Finalize();
    LOG(INFO) << "全部 " << app.configs_.size() << " 张 sun path gold image 生成完毕";
    return 0;
}

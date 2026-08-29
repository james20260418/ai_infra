// JPOV 太阳轨迹 + 辉光（bloom）gold image generator
//
// 渲染 sun_path 的黄昏时段（index=1）在 Bloom 开/关两种状态下的效果图，
// 写仓库 gold image：
//   - sun_path_bloom_1.png      （bloom on，太阳盘周围有光晕）
//   - sun_path_nobloom_1.png    （bloom off，同一场景作为对照）
//
// 相机 + 场景复用 sun_path（StandardSunnyDay 布景 + 天光），光照由
// DefaultSunPath() 的 index=1（低仰角黄昏）驱动。Bloom 参数见 DefaultBloom()。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_sun_path_bloom_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV Sun Path Bloom Gold Generator";
    cfg.headless = true;
    jpov_sun_path_bloom::SunPathBloomApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);
    app.configs_ = jpov_sun_path::DefaultSunPath();
    app.bloom_cfg = jpov_sun_path_bloom::DefaultBloom();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    const int kDuskIndex = 1;  // 黄昏时段（sun_path index=1）
    CHECK_LT(kDuskIndex, static_cast<int>(app.configs_.size()));

    app.active_index_ = kDuskIndex;

    // Gold 1：bloom 开。
    app.bloom_enabled = true;
    {
        std::string out = jpov::GetTestDataDir()
            + "/object3d/skydome/sun_path_bloom_1.png";
        app.RunOnce(input, winfo, out.c_str());
        LOG(INFO) << "bloom ON  gold: " << out;
    }

    // Gold 2：bloom 关（同一场景，作为对照）。
    app.bloom_enabled = false;
    {
        std::string out = jpov::GetTestDataDir()
            + "/object3d/skydome/sun_path_nobloom_1.png";
        app.RunOnce(input, winfo, out.c_str());
        LOG(INFO) << "bloom OFF gold: " << out;
    }

    app.Finalize();
    LOG(INFO) << "sun path bloom gold images 生成完毕";
    return 0;
}

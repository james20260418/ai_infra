// JPOV Fog 点光源雾散射 gold image generator
//
// 用固定场景 + 固定参数（见 fog_gold_common.h 的 FogPointLightGoldApp）渲染一张图，
// 写入仓库内 gold：
//   tools/jpov/test/fog/fog_pointlight_1280x720.png
// 供 jpov_fog_pointlight_gold_test 比对 + 肉眼查看。
//
// 运行：
//   bazel run //tools/jpov/test/fog:jpov_fog_pointlight_gold_generator

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/fog/fog_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath = jpov::GetTestDataDir() + "/fog/" +
                                jpov_fog_gold::kPointLightGoldPngName;

    JPOV::Config cfg;
    cfg.title = "JPOV Fog Point-Light Gold Generator";
    cfg.headless = true;

    jpov_fog_gold::FogPointLightGoldApp app(cfg);
    app.Init();
    app.Install();

    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(jpov_fog_gold::kResW);
    winfo.height = static_cast<float>(jpov_fog_gold::kResH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}

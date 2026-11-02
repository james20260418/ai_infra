// JPOV SSR gold image generator（5 个固定组合）
//
// 产物：tools/jpov/test/ssr/ssr_gold_<case>_1280x720.png
// 供 jpov_ssr_gold_test 做端到端保护（组合定义见 jpov_ssr_gold_common.h）。
//
// 用法：bazel run //tools/jpov/test/ssr:jpov_ssr_gold_generator

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/ssr/jpov_ssr_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV SSR Gold Generator";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.Init();
    jpov_ssr_gold::AssembleScene(app);

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    const std::string dir = jpov::GetTestDataDir() + "/ssr/";
    for (const jpov_ssr_gold::SsrGoldCase& c : jpov_ssr_gold::kCases) {
        jpov_ssr_gold::ApplyCase(app, c);
        const std::string path = dir + jpov_ssr_gold::GoldFileName(c);
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "gold generated: " << path;
    }
    app.Finalize();
    return 0;
}

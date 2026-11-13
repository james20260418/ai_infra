// JPOV SSR gold image generator（11 个固定组合：5 基础 + 6 能力）
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

    // 调机位用：SSR_GOLD_AB_DIR=<dir> 时，每个组合额外出一张「六项全关」的对照图，
    // 方便确认这一项在该机位下**真的起作用**（gold 自身的 A/B 断言见 jpov_ssr_gold_test）。
    const char* ab_env = std::getenv("SSR_GOLD_AB_DIR");
    const std::string ab_dir = (ab_env != nullptr) ? ab_env : "";
    if (!ab_dir.empty()) {
        std::system(("mkdir -p " + ab_dir).c_str());
    }

    const std::string dir = jpov::GetTestDataDir() + "/ssr/";
    for (const jpov_ssr_gold::SsrGoldCase& c : jpov_ssr_gold::kCases) {
        jpov_ssr_gold::ApplyCase(app, c);
        const std::string path = dir + jpov_ssr_gold::GoldFileName(c);
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "gold generated: " << path;

        if (!ab_dir.empty()) {
            jpov_ssr_gold::DisableAllFeatures(app);
            const std::string off = ab_dir + "/" + c.name + "_off.png";
            app.RunOnce(input, winfo, off.c_str());
            LOG(INFO) << "   [AB] off variant: " << off;
        }
    }
    app.Finalize();
    return 0;
}

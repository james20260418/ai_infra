// JPOV 资产骨架 gold generator —— 写仓库 gold image
//
// 与 test(jpov_asset_skeleton_gold_test) 共用 jpov_asset_skeleton_gold_common.h 场景：
// 浅灰地面 + 标准晴天 + 两根"从资产读出的" identity 骨人（glb 红 / fbx 蓝）。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/skeleton/jpov_asset_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

static std::string TestDataDir() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') s.push_back('/');
        return s + "__main__/tools/jpov/test";
    }
    return jpov::GetTestDataDir();
}

int main() {
    const std::string outpath =
        TestDataDir() + jpov_asset_skeleton_gold::GetGoldRelPath();

    JPOV::Config cfg;
    cfg.title = "JPOV Asset Skeleton Gold (glb + fbx)";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_asset_skeleton_gold::AssetSkeletonGoldApp app(cfg);
    app.Init();
    jpov_asset_skeleton_gold::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    LOG(INFO) << "asset skeleton gold generated: " << outpath;

    app.Finalize();
    return 0;
}

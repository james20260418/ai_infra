// JPOV 3D 文本深度/alpha gold 生成器。
//
// 与 jpov_text3d_depth_alpha_gold_test.cc 共用 text3d_depth_alpha_common.h 的
// 场景（必须逐字节一致）。
//
// 用法（repo 根）：
//   bazel run //tools/jpov/test/object3d:text3d_depth_alpha_gold_generator
// 产物：tools/jpov/test/object3d/text3d_depth_alpha_1280x720.png
//
// ⚠️ 改场景（相机/文字/颜色/字体/锚点）后必须重跑本生成器，否则 test 会红。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/text3d_depth_alpha_common.h"
#include "tools/jpov/test/test_utils.h"

int main(int argc, char** argv) {
    using namespace jpov_text3d_depth_alpha;

    std::string outpath;
    if (argc >= 2) {
        outpath = argv[1];
    } else {
        outpath = jpov::GetTestDataDir() + "/object3d/" + GetGoldName();
    }
    LOG(INFO) << "gold 输出: " << outpath;

    JPOV::Config cfg;
    cfg.title = "3D Text Depth+Alpha Gold Generator";
    cfg.headless = true;
    cfg.width = kOutW;
    cfg.height = kOutH;
    // 中文字体：LxgwWenKai 是 .ttf（TrueType outline，stb_truetype 稳）。
    cfg.fonts = {{"tools/jpov/fonts/LxgwWenKai-Regular.ttf", 0, kFontAlias}};

    Text3dDepthAlphaApp app(cfg);
    app.Init();
    BuildText3dDepthAlphaScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(kOutW);
    winfo.height = static_cast<float>(kOutH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "gold 生成完成（须与 test 场景逐字节一致）";
    return 0;
}

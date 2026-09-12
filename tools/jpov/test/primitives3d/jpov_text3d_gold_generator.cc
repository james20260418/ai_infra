// JPOV 3D 文本 gold 生成器。
//
// 与 jpov_text3d_gold_test.cc 共用同一场景（必须逐字节一致）：
//   跑一次渲染，把结果写到仓库内的 gold PNG。
//
// 用法（在 repo 根目录）：
//   bazel run //tools/jpov/test/primitives3d:jpov_text3d_gold_generator
// 产物：tools/jpov/test/primitives3d/text3d_quad_1280x720.png
//
// ⚠️ 改场景（相机/文字/颜色/字体）后必须重跑本生成器，否则 test 会红。

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

namespace {

constexpr int kFboW = 1280;
constexpr int kFboH = 720;

// 与 test 完全一致（改一处必须改两处，见 test 文件头）。
class Text3dGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = static_cast<float>(kFboW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kFboH);
        cmds->camera.position = {0.0f, 0.0f, 6.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.1f;
        cmds->camera.far      = 100.0f;

        cmds->tone_mapping = false;

        cmds->DrawText3D("JPOV", {0.0f, 1.5f, 0.0f},
                         {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f},
                         0.9f, jpov::kColorRed, "", jpov::TextAlignment::kCenter);

        cmds->DrawText3D("ABC", {-2.0f, 0.0f, 0.0f},
                         {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                         0.9f, jpov::kColorGreen, "",
                         jpov::TextAlignment::kCenter);

        cmds->DrawText3D("3D", {2.0f, 0.0f, 0.0f},
                         {0.0f, 0.0f, 1.0f}, {0.4f, 1.0f, 0.3f},
                         0.9f, jpov::kColorBlue, "",
                         jpov::TextAlignment::kCenter);
    }
};

}  // namespace

int main(int argc, char** argv) {
    // 输出路径：默认写仓库 gold 路径；可用 argv[1] 覆盖（调试用）。
    std::string outpath;
    if (argc >= 2) {
        outpath = argv[1];
    } else {
        outpath = jpov::GetProjectRoot() +
                  "tools/jpov/test/primitives3d/text3d_quad_1280x720.png";
    }
    LOG(INFO) << "gold 输出: " << outpath;

    JPOV::Config cfg;
    cfg.title = "3D Text Gold Generator";
    cfg.headless = true;
    cfg.width = 640;
    cfg.height = 360;
    cfg.fonts = {{"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin}};

    Text3dGoldApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "gold 生成完成（须与 test 场景逐字节一致）";
    return 0;
}

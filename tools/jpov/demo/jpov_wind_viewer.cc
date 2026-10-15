// JPOV 植被风查看器（wind viewer）— main。
//
// 模特：小柳树（willow_small_tex.glb，加载时派生 bend/phase）。
// 交互：面板调风（方位/风速/振幅/频率/湍流/株距/扰动/株数/时间）。
// headless：--capture-wind <dir> [--distance <r>] [--frames <n>]
//   出图 wind_off.png（关风）/ wind_on_t00..（每秒一帧）。
//
// 设计：tools/jpov/docs/jpov_vegetation_wind_design.md。

#include <cstdio>
#include <cstring>
#include <string>

#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/demo/wind_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

JPOV::Config MakeConfig(const char* title, bool headless) {
    JPOV::Config cfg;
    cfg.title    = title;
    cfg.width    = jpov_wind::kViewerWidth;
    cfg.height   = jpov_wind::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = static_cast<int>(jpov_wind::kViewerFps);
    cfg.headless = headless;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",          0, jpov::kFontBuiltinLatin},
    };
    return cfg;
}

// headless 拍摄：固定机位，先出「关风」参照，再逐秒出「开风」帧。
int RunWindCapture(const std::string& out_dir, float distance, int frames) {
    jpov_wind::WindViewerApp app(MakeConfig("JPOV — 植被风 A/B（headless）",
                                            /*headless=*/true));
    app.SetShowPanel(false);
    app.Init();
    app.InstallScene();

    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(jpov_wind::kViewerWidth);
    winfo.height = static_cast<float>(jpov_wind::kViewerHeight);
    const jpov::InputSnapshot input{};

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.16f;
    app.view_.theta = 3.14159265358979323846f / 4.0f;
    app.view_.R     = distance;

    // 关风参照。
    app.SetWindEnabled(false);
    app.SetFixedTime(0.0f);
    app.RunOnce(input, winfo, (out_dir + "/wind_off.png").c_str());

    // 开风：每秒一帧（确定性 —— 固定时间）。
    app.SetWindEnabled(true);
    for (int f = 0; f < frames; ++f) {
        app.SetFixedTime(static_cast<float>(f));
        char name[512];
        std::snprintf(name, sizeof(name), "%s/wind_on_t%02d.png", out_dir.c_str(), f);
        app.RunOnce(input, winfo, name);
    }
    LOG(INFO) << "wind capture → " << out_dir << " (" << frames << " frames)";
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string wind_dir;
    float distance = 12.0f;
    int frames = 4;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture-wind") == 0 && i + 1 < argc) {
            wind_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--distance") == 0 && i + 1 < argc) {
            distance = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::atoi(argv[i + 1]);
        }
    }
    if (!wind_dir.empty()) {
        return RunWindCapture(wind_dir, distance, frames);
    }

    // ── 交互模式 ──
    jpov_wind::WindViewerApp app(MakeConfig("JPOV — 植被风查看器（小柳树）",
                                            /*headless=*/false));
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    app.InstallScene();

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.16f;
    app.view_.theta = 3.14159265358979323846f / 4.0f;
    app.view_.R     = 12.0f;

    app.Run();
    app.Finalize();
    return 0;
}

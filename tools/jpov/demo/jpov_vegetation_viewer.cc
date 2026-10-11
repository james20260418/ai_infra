// JPOV 植被查看器（vegetation viewer）— 主程序。
//
// 用途：肉眼验收 **L-System 生成的植被拓扑骨架**（MVP：每根枝画成一根空间棒棒）。
// 树种为内置「龙爪槐」。视角变换与 model/skylight viewer 相同（右键拖拽/滚轮缩放）。
//
// 交互面板 4 个 knob（改动即重建骨架，确定性）：展开代数 / 长度缩放 / 基本粗细 / 随机种子。
//
// headless 拍摄（--capture <out_dir>）：输出一组「生长扫谱」图（代数递增）+ 一张成品
// 三视角，供交付验收 / 自动化核对。
//
// 架构：与 skylight viewer 同构（主程序只做装配 + 事件循环；渲染核心 + 面板在
// vegetation_viewer_app.h）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   ./tools/jpov/build_jpov_vegetation_viewer.sh
//   → output/jpov_vegetation_viewer/jpov_vegetation_viewer

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/vegetation_viewer_app.h"
#include "tools/jpov/demo/view_config.h"

namespace {

using jpov_vegetation::VegetationApp;

constexpr int kWidth  = jpov_vegetation::kViewerWidth;
constexpr int kHeight = jpov_vegetation::kViewerHeight;
constexpr double kPi  = 3.14159265358979323846;

// 交互窗口 / 拍摄共用的字体声明（路径相对 exe 的 fonts/，build 脚本同构拷贝）。
std::vector<JPOV::FontEntry> ViewerFonts() {
    return {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",          0, jpov::kFontBuiltinLatin},
    };
}

// headless 拍摄：无窗口批量出图。
int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被查看器（headless capture）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(false);   // 纯 3D 截图
    app.Init();
    app.InstallScene();

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    auto shoot = [&](const std::string& name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // ── A. 生长扫谱：代数递增，看骨架从主干长成完整树冠 ──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0;
    for (int gen : {2, 5, 8, 12, 16, 20, 25}) {
        app.generations_ = gen;
        app.RegenerateNow();
        shoot("growth_gen" + std::to_string(gen));
    }

    // ── B. 成品三视角（代数 25）──
    app.generations_ = 25;
    app.RegenerateNow();
    struct ViewAngle { const char* name; float phi; float theta; float R; };
    const ViewAngle kAngles[] = {
        {"hero",   0.12f, 0.7853982f, 8.0f},
        {"front", 0.05f, 0.0f,        8.0f},
        {"side",  0.05f, 1.5707963f, 8.0f},
        {"closeup", 0.05f, 0.7853982f, 4.0f},
        {"top",   1.30f, 0.7853982f, 8.0f},
    };
    for (const ViewAngle& va : kAngles) {
        app.view_.phi   = va.phi;
        app.view_.theta = va.theta;
        app.view_.R     = va.R;
        shoot(std::string("dragon_claw_") + va.name);
    }

    // ── C. 叶片生长扫谱（代数 25，叶大小随 growth 张开；同机位）──
    app.view_.phi   = 0.12f;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0f;
    for (float g : {0.15f, 0.4f, 0.7f, 1.0f}) {
        app.leaf_growth_ = g;
        app.RegenerateNow();
        shoot("leafgrowth_" + std::to_string(static_cast<int>(g * 100)));
    }

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir);
    }

    // ── 交互模式 ──
    JPOV::Config cfg;
    cfg.title      = "JPOV — 植被查看器（L-System 龙爪槐）";
    cfg.width      = kWidth;
    cfg.height     = kHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_vegetation::kViewerFps);
    cfg.headless   = false;
    cfg.fonts      = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    app.InstallScene();

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0;

    app.Run();
    app.Finalize();
    return 0;
}

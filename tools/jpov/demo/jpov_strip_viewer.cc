// JPOV 半透明条带查看器 — 主程序
//
// 场景 = 天光 + 灰色地面 + 一批三条带（**一次 draw call**，互有重叠以验收混合）。
// 条带纹理：assets/strip/ribbon_test.png（Danis 手绘；近黑像素已转 alpha=0）。
//
// 交互（需 DISPLAY / WSLg）： ./output/jpov_strip_viewer/jpov_strip_viewer
// headless 拍摄：           ... /jpov_strip_viewer --capture <out_dir>
// 编译：                    ./tools/jpov/build_jpov_strip_viewer.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/demo/strip_viewer_app.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

#ifndef _WIN32
#include <unistd.h>
std::string ExeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const std::string p(buf);
        const size_t s = p.find_last_of('/');
        if (s != std::string::npos) {
            return p.substr(0, s);
        }
    }
    return std::string();
}
#else
std::string ExeDir() { return std::string(); }
#endif

// 条带纹理查找顺序：构建产物目录 → 可执行文件目录 → 仓库 assets。
std::string ResolveTexturePath() {
    const std::string d = ExeDir();
    const char* rels[] = {"textures/ribbon_test.png", "ribbon_test.png",
                          "../assets/strip/ribbon_test.png"};
    for (const char* r : rels) {
        const std::string p = d.empty() ? std::string(r) : (d + "/" + r);
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f != nullptr) {
            std::fclose(f);
            return p;
        }
    }
    return std::string();
}

// headless 拍摄：固定视角/相位，输出三张（默认 / 无纹理 / 硬边）便于对照。
int RunCapture(const std::string& out_dir, const std::string& texture_path) {
    JPOV::Config cfg;
    cfg.title = "JPOV - strip viewer (capture)";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_strip::StripViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.Setup(texture_path);

    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    jpov_strip::ApplyDefaultView(&app);

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    shoot("10_default");
    app.show_strips_ = false;
    shoot("15_no_strips");
    app.show_strips_ = true;
    app.use_texture_ = false;
    shoot("20_solid");
    app.use_texture_ = true;
    app.soft_edges_ = false;
    shoot("30_hard_edges");
    app.soft_edges_ = true;
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    shoot("99_panel");
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

    const std::string texture_path = ResolveTexturePath();
    if (texture_path.empty()) {
        LOG(WARNING) << "找不到条带纹理 assets/strip/ribbon_test.png"
                        "（先跑 ./tools/jpov/build_jpov_strip_viewer.sh）";
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, texture_path);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — 半透明条带查看器（strip viewer）";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = 60;
    cfg.headless = false;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_strip::StripViewerApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    app.Setup(texture_path);
    jpov_strip::ApplyDefaultView(&app);
    app.Run();
    app.Finalize();
    return 0;
}

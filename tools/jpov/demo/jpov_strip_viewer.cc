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

// headless 拍摄：固定视角/相位，按下面的**拍摄表**逐张出图（A/B 对照用）。
//
// 每行只写「与默认值不同的开关」—— 不写增量、也不用手动还原，
// 漏还原一个开关会静默污染后面所有图（这正是上一版的问题）。
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

    // 一张图的完整开关；默认值 = 交互模式的初始状态（只写差异，读起来就是"这张在测什么"）。
    struct Shot {
        const char* name;
        bool show_strips = true;     // false = 不发条带命令（A/B 对照）
        bool use_texture = true;     // false = 纯色（不看纹理）
        bool soft_edges = true;      // 宽度剖面：柔边 / 硬边
        bool arc_additive = false;   // 弧形改走加法集（发光拖尾）
        bool soft_depth = false;     // 条带与几何相交处软过渡
        bool smooth = false;         // false = 折线 pass；true = 平滑 pass（kSmooth）
        float camera_r = jpov_strip::kViewDefaults.R;
    };
    const float kZoomR = 2.6f;   // 拉近：16 段的折角/轮廓只有近距离才看得清
    const Shot kShots[] = {
        {.name = "10_default"},
        {.name = "15_no_strips", .show_strips = false},
        {.name = "20_solid", .use_texture = false},
        {.name = "30_hard_edges", .soft_edges = false},
        {.name = "40_arc_additive", .arc_additive = true},
        {.name = "50_soft_depth", .soft_depth = true},
        {.name = "60_zoom_soft", .camera_r = kZoomR},
        {.name = "65_zoom_hard", .soft_edges = false, .camera_r = kZoomR},
        // 平滑 pass：同一组视角/开关，只换 style（与 60/65/10 一一对应）。
        {.name = "70_smooth_zoom_soft", .smooth = true, .camera_r = kZoomR},
        {.name = "75_smooth_zoom_hard", .soft_edges = false, .smooth = true,
         .camera_r = kZoomR},
        {.name = "80_smooth_default", .smooth = true},
    };
    for (const Shot& shot : kShots) {
        app.show_strips_ = shot.show_strips;
        app.use_texture_ = shot.use_texture;
        app.soft_edges_ = shot.soft_edges;
        app.arc_additive_ = shot.arc_additive;
        app.soft_depth_ = shot.soft_depth;
        app.smooth_strips_ = shot.smooth;
        app.view_.R = shot.camera_r;
        shoot(shot.name);
    }

    // 面板单独一张（HUD 文案能证明当前是哪个 pass）：字体必须先量好。
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    app.smooth_strips_ = false;
    app.view_.R = jpov_strip::kViewDefaults.R;
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

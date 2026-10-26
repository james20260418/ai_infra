// JPOV 半透明条带查看器 — 主程序
//
// 场景 = 天光 + 灰色地面 + 参考方块 + 一批三条带（**一次 draw call**）+ 玻璃圆环
//（四组点 → 四条带，另一个 batch）。条带纹理：assets/strip/ribbon_test.png
//（Danis 手绘；近黑像素已转 alpha=0）。
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

// headless 拍摄：固定视角/相位，按下面的**拍摄表**逐张出图（AB/预设对照用）。
//
// 每行只写「与默认值不同的开关」—— 不写增量、也不用手动还原，
// 漏还原一个开关会静默污染后面所有图（这正是上一版的问题）。
// 圆环 + 菲涅耳的对照图：100（无边反基线）~ 150（五个预设）、160/170（折线 vs 平滑）。
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
    // 玻璃缸场景的橡树在**首帧之前**加载（帧中上传 GL 资源会撞 1282）。
    // 只有拍摄表里用到它时才加载，gold 的 8 张图完全不付这份成本。
    app.PrepareTankScene();

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
    //
    // 每张先选场景（见 StripViewerApp::SelectScene），再写「与场景默认不同的开关」。
    // 玻璃圆环（ring）不在任何场景里，它的 fresnel/环几何代码仍由 gold 覆盖。
    struct Shot {
        const char* name;
        int scene = 0;   // 0 = box+三条带 / 1 = 射流 / 2 = 玻璃缸+树
        // 其余全部是**可选覆盖**：只写「与场景预设不同」的开关（不写 = 用场景预设）。
        // 用 optional 而不是带默认值的 bool：场景自己会写一遍可见性开关，如果这里再写一遍
        // 「缺省 true」，就会把场景预设覆盖掉（第一版就踩了：射流场景里三条带和方块又冒出来）。
        std::optional<bool> show_strips;
        std::optional<bool> ribbons;
        std::optional<bool> use_texture;
        std::optional<bool> texture_mipmap;
        std::optional<bool> soft_edges;
        std::optional<bool> arc_additive;
        std::optional<bool> soft_depth;
        std::optional<bool> smooth;
        std::optional<bool> fresnel;
        std::optional<bool> bloom;
        std::optional<float> jet_gain;
        std::optional<float> camera_r;
        // 玻璃缸场景
        std::optional<bool> tank_skylight;
        std::optional<float> sun_elev_deg;
        std::optional<float> tank_injection;
    };
    const float kZoomR = 2.6f;   // 拉近：16 段的折角/轮廓只有近距离才看得清
    const Shot kShots[] = {
        // ── 场景 ①：box + 三条带（条带本体 / 纹理 / 剖面 / 加法集 / 软深度）──
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
        {.name = "75_smooth_zoom_hard", .soft_edges = false, .smooth = true, .camera_r = kZoomR},
        {.name = "80_smooth_default", .smooth = true},
        // 远机位纹理 A/B：mip 关 ↔ 开（看远处细纹理的闪烁 vs 模糊）。不入 gold。
        {.name = "300_far_tex_no_mip", .smooth = true, .camera_r = 14.0f},
        {.name = "305_far_tex_mip", .texture_mipmap = true, .smooth = true,
         .camera_r = 14.0f},

        // ── 场景 ②：射流（加法集 HDR + 逐点颜色 + 中心实两侧虚剖面 + bloom）──
        {.name = "200_jet_smooth", .scene = 1, .smooth = true},
        {.name = "205_jet_polyline", .scene = 1, .smooth = false},
        {.name = "210_jet_no_bloom", .scene = 1, .smooth = true, .bloom = false},
        {.name = "215_jet_gain8", .scene = 1, .smooth = true, .jet_gain = 8.0f},

        // ── 场景 ③：玻璃缸 + 树（天光消费）──
        {.name = "400_tank_unlit", .scene = 2, .tank_skylight = false},
        {.name = "405_tank_skylit", .scene = 2},
        {.name = "410_tank_skylit_lowsun", .scene = 2, .sun_elev_deg = 8.0f},
        {.name = "415_tank_skylit_night", .scene = 2, .sun_elev_deg = -8.0f},
        {.name = "420_tank_skylit_inj40", .scene = 2, .tank_injection = 0.40f},
        // 边反 A/B（原来由玻璃圆环承担，现在缸壁同样吃 StripFresnel）
        {.name = "430_tank_no_fresnel", .scene = 2, .fresnel = false},
        {.name = "435_tank_fresnel", .scene = 2, .fresnel = true},
    };
    for (const Shot& shot : kShots) {
        // 先选场景（写可见性开关 + 该场景的默认视角），再应用**显式覆盖**。
        app.SelectScene(shot.scene);
        if (shot.show_strips) { app.show_strips_ = *shot.show_strips; }
        if (shot.ribbons) {
            app.vis_ = jpov_strip::RibbonVisibility{*shot.ribbons, *shot.ribbons, *shot.ribbons};
        }
        if (shot.use_texture) { app.use_texture_ = *shot.use_texture; }
        if (shot.texture_mipmap) { app.texture_mipmap_ = *shot.texture_mipmap; }
        if (shot.soft_edges) { app.soft_edges_ = *shot.soft_edges; }
        if (shot.arc_additive) { app.arc_additive_ = *shot.arc_additive; }
        if (shot.soft_depth) { app.soft_depth_ = *shot.soft_depth; }
        if (shot.smooth) { app.smooth_strips_ = *shot.smooth; }
        if (shot.fresnel) { app.fresnel_enabled_ = *shot.fresnel; }
        if (shot.bloom) { app.bloom_ = *shot.bloom; }
        if (shot.jet_gain) { app.jet_gain_ = *shot.jet_gain; }
        if (shot.camera_r) { app.view_.R = *shot.camera_r; }
        if (shot.tank_skylight) { app.tank_skylight_ = *shot.tank_skylight; }
        if (shot.sun_elev_deg) { app.deg_.sun_elev_deg = *shot.sun_elev_deg; }
        if (shot.tank_injection) { app.tank_injection_ = *shot.tank_injection; }
        shoot(shot.name);
    }

    // 面板：每个场景各拍一张（证明「参数随场景动态出现」+ 底板读得清）。
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    for (int scene = 0; scene < jpov_strip::StripViewerApp::kSceneCount; ++scene) {
        app.SelectScene(scene);       // 可见性开关由场景写，这里只改机位
        app.view_.theta = 0.45;
        const std::string name = std::string("98_panel_") + std::to_string(scene);
        shoot(name.c_str());
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
    // 交互模式同理：必须在 Run() 之前加载（面板上的勾选只是开关，不负责加载）。
    // 代价 = 启动时多一次 4k glb 解压/上传（约 1s）；换来勾选「显示玻璃缸」零延迟。
    app.PrepareTankScene();
    jpov_strip::ApplyDefaultView(&app);
    app.Run();
    app.Finalize();
    return 0;
}

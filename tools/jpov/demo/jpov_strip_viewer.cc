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
    // 每一行都**完整**覆盖所有开关（不靠上一张的残留状态）：漏还原一个开关会把后面所有图静默污染。
    struct Shot {
        const char* name;
        bool show_strips = true;     // false = 不发条带命令（A/B 对照）
        bool ribbons = true;         // 三条带（波/螺旋/弧）
        bool ring = true;            // 玻璃圆环（四组点 → 四条带）
        bool use_texture = true;     // false = 纯色（不看纹理）
        bool soft_edges = true;      // 宽度剖面：柔边 / 硬边（圆环不受它影响，见 EmitRingBatch）
        bool arc_additive = false;   // 弧形改走加法集（发光拖尾）
        bool soft_depth = false;     // 条带与几何相交处软过渡
        bool smooth = false;         // false = 折线 pass；true = 平滑 pass（kSmooth）
        bool fresnel = true;         // 菲涅耳边反开关
        int fresnel_preset = 0;      // kFresnelPresets 下标（fresnel=false 时不生效）
        bool ring_at_origin = false; // 圆环挪到原点：只看圆环时居中
        bool boxes = true;           // 背景参考方块（只看圆环时关掉，免得透过玻璃挡住环体）
        bool jet = true;             // 火箭射流（加法集 + HDR 增益 + 逐点颜色）
        float jet_gain = 3.0f;       // 射流 HDR 增益
        bool bloom = true;           // 辉光（HDR 高亮够亮才看得出来）
        float camera_r = jpov_strip::kViewDefaults.R;
    };
    const float kZoomR = 2.6f;   // 拉近：16 段的折角/轮廓只有近距离才看得清
    const float kRingR = 3.6f;   // 只看圆环时的机位（充满画面又不贴脸）
    // 注意 10~50 是**折线 pass** 的基线（M1~M3 的特性验收）；viewer 交互模式的默认
    // 现在是平滑 pass（80 / 200 / 220 是平滑版本），折线仍可用面板切回来做 A/B。
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
        // 圆环 + 菲涅耳：只看圆环（隐藏三条带、圆环居中），逐个预设出图对比。
        // 100 是"没有边反"的基线：同一机位下的淡蓝薄膜，后面每张只差 fresnel 参数。
        {.name = "100_ring_no_fresnel", .ribbons = false, .fresnel = false,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        {.name = "110_fresnel_physical", .ribbons = false, .fresnel_preset = 0,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        {.name = "120_fresnel_plastic", .ribbons = false, .fresnel_preset = 1,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        {.name = "130_fresnel_thick", .ribbons = false, .fresnel_preset = 2,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        {.name = "140_fresnel_mirror", .ribbons = false, .fresnel_preset = 3,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        {.name = "150_fresnel_narrow", .ribbons = false, .fresnel_preset = 4,
         .ring_at_origin = true, .boxes = false, .camera_r = kRingR},
        // 平滑 pass 的解析法线（无折面）vs 折线 pass 的导数法线（逐折面亮带）
        {.name = "160_fresnel_polyline_near", .ribbons = false, .fresnel_preset = 0,
         .ring_at_origin = true, .boxes = false, .camera_r = 2.4f},
        {.name = "170_fresnel_smooth_near", .ribbons = false, .smooth = true,
         .fresnel_preset = 0, .ring_at_origin = true, .boxes = false, .camera_r = 2.4f},
        // 默认场景（三条带 + 圆环同屏）：看边反在完整场景里的观感。
        {.name = "180_scene_with_ring"},
        {.name = "185_scene_smooth_ring", .smooth = true},
        // 射流：同时验「加法集 HDR 增益 + 逐点颜色（白→红）+ 中心实两侧虚剖面」。
        // 200 平滑 / 205 折线是同一机位的 style A/B（射流没有大折角，差别主要在软边上）。
        {.name = "200_jet_smooth", .ribbons = false, .ring = false, .smooth = true},
        {.name = "205_jet_polyline", .ribbons = false, .ring = false},
        {.name = "210_jet_no_bloom", .ribbons = false, .ring = false, .smooth = true,
         .bloom = false},
        {.name = "215_jet_gain8", .ribbons = false, .ring = false, .smooth = true,
         .jet_gain = 8.0f},
        {.name = "220_scene_with_jet", .smooth = true},
    };
    for (const Shot& shot : kShots) {
        CHECK(shot.fresnel_preset >= 0 && shot.fresnel_preset < jpov_strip::kFresnelPresetCount)
            << "拍摄表的 fresnel_preset 越界: " << shot.fresnel_preset;
        app.show_strips_ = shot.show_strips;
        app.vis_ = jpov_strip::RibbonVisibility{shot.ribbons, shot.ribbons, shot.ribbons};
        app.ring_visible_ = shot.ring;
        app.boxes_visible_ = shot.boxes;
        app.jet_visible_ = shot.jet;
        app.jet_gain_ = shot.jet_gain;
        app.bloom_ = shot.bloom;
        app.use_texture_ = shot.use_texture;
        app.soft_edges_ = shot.soft_edges;
        app.arc_additive_ = shot.arc_additive;
        app.soft_depth_ = shot.soft_depth;
        app.smooth_strips_ = shot.smooth;
        // 圆环的平滑开关独立：拍摄表里沿用同一个 shot.smooth，
        // 于是 100~150 仍是折线、160/170 才是圆环的折线 vs 平滑对照。
        app.ring_smooth_ = shot.smooth;
        app.fresnel_enabled_ = shot.fresnel;
        app.fresnel_preset_ = shot.fresnel_preset;
        app.fresnel_ = jpov_strip::kFresnelPresets[shot.fresnel_preset].params;
        app.ring_shape_ = jpov_strip::MakeSceneRingShape();
        if (shot.ring_at_origin) {
            app.ring_shape_.center_x = 0.0f;
            app.ring_shape_.center_z = 0.0f;
            // 抬到参考方块之上（方块顶 0.995）：居中看圆环时不想让方块从环体里穿过去。
            app.ring_shape_.center_y = 1.75f;
        }
        app.view_.R = shot.camera_r;
        shoot(shot.name);
    }

    // 面板单独一张（HUD 文案能证明当前是哪个 pass / 哪套 fresnel 参数）：字体必须先量好。
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    app.smooth_strips_ = false;
    app.ring_smooth_ = true;
    app.vis_ = jpov_strip::RibbonVisibility{};
    app.ring_visible_ = true;
    app.fresnel_enabled_ = true;
    app.fresnel_preset_ = 0;
    app.ring_shape_ = jpov_strip::MakeSceneRingShape();
    app.jet_visible_ = true;
    app.jet_gain_ = 3.0f;
    app.bloom_ = true;
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

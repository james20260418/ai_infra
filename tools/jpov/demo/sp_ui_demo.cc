// JPOV sp_ui demo — SDF 板材版即时模式 UI 调试台
//
// 目的（Danis 2026-10-27）：用 **Plate（SDF 圆角矩形 + 边框剖面 + 材质平铺/翻书）**
// 复刻旧 ui 已经有的**全部控件**，作为 sp_ui 基础能力的验收台。
//
// 与 jpov_ui_demo 的关系：布局是**同一份代码复制而来**（sp_ui_demo_panel.h 从
// ui_demo_panel.h 复制），控件动线、命中判定、状态槽逐行相同。差异只有两处：
//   1. 控件底板从「CPU 三角化的内外环」换成 `Plate2DCommand`（SDF）；
//   2. 底板多了一条材质链：大理石 flipbook 平铺 + tint + 边框剖面。
// 因此两边可以逐控件并排对照 ——「复刻」是可验证的，不是感觉像。
//
// 两个运行模式：
//   交互（需 DISPLAY / WSLg）：
//     ./output/jpov_sp_ui_demo/jpov_sp_ui_demo
//   headless 拍摄（每个时间点一张 PNG，时间轴固定 ⇒ 可复现、可做 gold）：
//     ./output/jpov_sp_ui_demo/jpov_sp_ui_demo --capture shots --times 0,0.4,0.8
//
// 编译：
//   ./tools/jpov/build_jpov_sp_ui_demo.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/sp_ui_demo_panel.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/sp_ui/sp_ui.h"

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

// 材质查找顺序：构建产物目录 → 可执行文件目录 → 仓库 assets。
// 与 strip viewer 同一套约定（产物目录里由 build 脚本同构拷贝）。
std::string ResolveMarbleTexture() {
    const std::string d = ExeDir();
    const char* rels[] = {
        "textures/plate_marble_flipbook_4x4.png",
        "plate_marble_flipbook_4x4.png",
        "../assets/sp_ui/marble/plate_marble_flipbook_4x4.png",
    };
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

// 本 demo 显式注册两个字体：CJK（NotoSansCJK，显示中文）+ Latin（DejaVu，拉丁）。
// JPOV 不提供隐式默认字体，必须在 Config::fonts 显式声明 path/alias。
static const char* const kDemoFontAlias = jpov::kFontBuiltinCJK;

// 板材外观参数（全部可由命令行覆盖，便于一次跑出多组对照）。
struct PlateOptions {
    float bump = 1.0f;        // 边框剖面曲率：0 = 平面带，1 = 圆管
    float fade = 0.0f;        // 渐变消失带宽（像素，[0, border]）
    float border = 3.0f;      // 边框带宽（像素）
    float radius = 6.0f;      // 圆角半径（像素）
    float material_px = 200.0f;  // 材质图集一格对应的屏幕像素（0 = 关闭，用 tiling 字面值）
    float flipbook_fps = 6.0f;   // 翻书帧率（0 = 静态）
    bool no_texture = false;  // 强制纯色（用于与旧 ui 逐像素对照）
    bool light = true;        // true = 大理石主题；false = 旧 ui 深色配色
    float scale = 1.5f;       // 整体 UI 缩放（1.5 = 线框、字号、内边距全部× 1.5）
};

class SpUiDemoApp : public JPOV {
public:
    using JPOV::JPOV;

    // 把 sp_ui 的文本测量回调接到真实字体进宽（同旧 ui demo，修复光标贴合）。
    static float DemoTextWidth(const char* text, float font_size,
                               const char* /*font_alias*/, void* userdata) {
        SpUiDemoApp* app = static_cast<SpUiDemoApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SpUiDemoApp::DemoTextWidth, this);
    }

    // 整体 UI 缩放（布局坐标 → 设备像素）。需在 RunOnce/Run 之前设。
    void SetUiScale(float scale) { ui_.SetUiScale(scale); }

    void SetMarbleTexture(uint32_t tex) { marble_tex_ = tex; }
    void SetPlateOptions(const PlateOptions& opt) { plate_opt_ = opt; }
    // 翻书时间（秒）。零点与步长由调用方定 —— RunOnce/交互都由这里写。
    void SetTimeSeconds(float t) { time_s_ = t; }
    // 交互模式下是否自己推进时间（拍摄模式恒 false：每张图的时间由 --times 指定）。
    void SetAutoAdvance(bool on) { auto_advance_ = on; }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)winfo;

        // 渲染分辨率 = 窗口分辨率 1280x720（面板与窗口 1:1，见面板文件）。
        cmds->camera.fbo_3d_width_ = jpov::sp_ui::kSpUiDemoWidth;
        cmds->camera.fbo_3d_height_ = jpov::sp_ui::kSpUiDemoHeight;

        // 主题 = 颜色（沿用旧 ui 的配色）+ 板材（材质/剖面）。
        jpov::sp_ui::SpTheme theme =
            jpov::sp_ui::SpTheme::Default(jpov::sp_ui::kSpUiDemoFontSize);
        if (plate_opt_.light) {
            // 大理石主题（默认）：底色是**压暗的大理石**（材质本身是白色，靠 tint 压下来）。
            // 为什么不用亮底：整个 UI 只有**一个** theme.foreground，标签文字落在面板底上、
            // 控件文字落在板材上 —— 两者亮度必须同向，才能用同一个文字色。
            // 亮石底 ⇒ 必须深字，但面板底也想亮的话就会变成“全亮底 + 深字”，
            // 旧 ui 的深色调子就没了。所以取「中暗石底 + 亮字 + 银边」，
            // 大理石纹路（拉伸后 std≈0.12）乘上 0.42 仍有 ±0.05 的可见起伏。
            theme.background = {0.16f, 0.17f, 0.19f, 1.0f};  // 面板底（平色，不走材质）
            theme.foreground = {0.95f, 0.95f, 0.96f, 1.0f};  // 文本/图标（亮字）
            theme.accent = {0.58f, 0.60f, 0.63f, 1.0f};      // 悬停（亮一档的石头）
            theme.selected = {0.28f, 0.31f, 0.36f, 1.0f};    // 按下（沉下去的深石）
            theme.hover = {0.42f, 0.43f, 0.45f, 1.0f};       // 控件默认底（中暗石）
            theme.border = {0.62f, 0.63f, 0.66f, 1.0f};      // 边框（银灰；不取 0.9 是为了不爆白）
            theme.disabled = {0.45f, 0.45f, 0.47f, 1.0f};
        }
        theme.font_alias = kDemoFontAlias;
        theme.corner_radius_px = plate_opt_.radius;
        theme.border_width_px = plate_opt_.border;

        // 板材：一场材质 + 一个剖面。所有控件（含装饰底）都从这里取。
        theme.plate_texture_id = plate_opt_.no_texture ? 0u : marble_tex_;
        theme.plate_material_px = plate_opt_.material_px;
        theme.plate_flipbook_fps = plate_opt_.flipbook_fps;
        theme.plate_tiles_x = 4;   // 图集 4x4（见 assets/sp_ui/marble/README.md）
        theme.plate_tiles_y = 4;
        theme.plate_border_bump = plate_opt_.bump;
        theme.plate_border_fade = plate_opt_.fade;

        // 帧时钟：log 时间戳 + 键盘 hold 阈值（固定 30fps 帧周期，与 target_fps 一致）。
        st_.frame = static_cast<int>(frame_count);
        const float frame_dt_ms = 1000.0f / 30.0f;

        // 材质时间：只有「时间驱动开关打开 + 交互模式」才自走。
        // 暂停 → 把当前秒数原封不动传给 Begin，即定格在当前帧（不是回到第 0 帧）。
        if (auto_advance_ && st_.anim_play) {
            time_s_ += frame_dt_ms / 1000.0f;
        }
        st_.time_s = static_cast<double>(time_s_);

        // 即时模式：每帧 Begin → 画整台 → End → Emit。
        // 第 5 个参数（frame_dt_ms）只影响键盘 hold 重复；
        // 第 6 个参数（time_seconds）只喂材质翻书 —— 引擎不取自己的时钟。
        // 视口/窗口尺寸乘同一个 scale：布局坐标仍是 1280×720 那一套。
        ui_.Begin(input, theme,
                  jpov::sp_ui::kSpUiDemoWidth * plate_opt_.scale,
                  jpov::sp_ui::kSpUiDemoHeight * plate_opt_.scale,
                  frame_dt_ms, time_s_);
        jpov::sp_ui::DrawSpUIDemoPanel(ui_, cmds, st_);
        ui_.End();
        ui_.Emit(cmds);
    }

private:
    jpov::sp_ui::SpUi ui_;              // 跨帧持有（焦点/下拉展开/拖动状态）
    jpov::sp_ui::SpDemoState st_;       // 跨帧持有（全部控件状态 + 实时 log + 帧时钟）
    uint32_t marble_tex_ = 0;
    PlateOptions plate_opt_;
    float time_s_ = 0.0f;
    bool auto_advance_ = false;
};

void PrintUsage() {
    std::printf(
        "用法：jpov_sp_ui_demo [options]\n"
        "\n"
        "  --capture <dir>      headless 拍摄模式：按 --times 每个时间点出一张 PNG\n"
        "  --times a,b,c        拍摄时间点（秒，默认 0,0.4,0.8,1.2）\n"
        "  --bump <0..1>        边框剖面曲率（默认 1.0 = 圆管；0 = 平面带）\n"
        "  --fade <px>          边框渐变消失带宽（默认 0；<= border 才有意义）\n"
        "  --border <px>        边框带宽（默认 3）\n"
        "  --radius <px>        圆角半径（默认 6）\n"
        "  --material-px <px>   材质图集一格对应的屏幕像素（默认 200；0 = 关闭）\n"
        "  --scale <n>          整体 UI 缩放（默认 1.5，窗口同步放大）\n"
        "  --fps <n>            翻书帧率（默认 6；0 = 静态材质）\n"
        "  --dark               用旧 ui 的深色配色（默认是浅色大理石主题）\n"
        "  --no-texture         强制纯色板材（与旧 ui 逐像素对照用）\n");
}

// 把 "a,b,c" 解析成浮点列表。解析失败 → 空列表（调用方 CHECK）。
std::vector<float> ParseFloatList(const char* s) {
    std::vector<float> out;
    if (s == nullptr) {
        return out;
    }
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) {
                out.push_back(std::strtof(cur.c_str(), nullptr));
                cur.clear();
            }
            if (*p == '\0') {
                break;
            }
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture_dir;
    std::vector<float> times = {0.0f, 0.4f, 0.8f, 1.2f};
    PlateOptions opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = (i + 1 < argc);
        if (a == "--help" || a == "-h") {
            PrintUsage();
            return 0;
        } else if (a == "--capture" && has_next) {
            capture_dir = argv[++i];
        } else if (a == "--times" && has_next) {
            times = ParseFloatList(argv[++i]);
            CHECK(!times.empty()) << "--times 解析结果为空";
        } else if (a == "--bump" && has_next) {
            opt.bump = std::strtof(argv[++i], nullptr);
        } else if (a == "--fade" && has_next) {
            opt.fade = std::strtof(argv[++i], nullptr);
        } else if (a == "--border" && has_next) {
            opt.border = std::strtof(argv[++i], nullptr);
        } else if (a == "--radius" && has_next) {
            opt.radius = std::strtof(argv[++i], nullptr);
        } else if (a == "--material-px" && has_next) {
            opt.material_px = std::strtof(argv[++i], nullptr);
        } else if (a == "--fps" && has_next) {
            opt.flipbook_fps = std::strtof(argv[++i], nullptr);
        } else if (a == "--scale" && has_next) {
            opt.scale = std::strtof(argv[++i], nullptr);
            CHECK_GT(opt.scale, 0.0f) << "--scale 必须 > 0";
        } else if (a == "--dark") {
            opt.light = false;
        } else if (a == "--no-texture") {
            opt.no_texture = true;
        } else {
            LOG(FATAL) << "未知参数：" << a << "（--help 看用法）";
        }
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — sp_ui 板材调试台";
    // 窗口 = 布局尺寸 × 缩放。布局仍按 1280×720 写，整体放大后线框/字号一起变粗。
    cfg.width = static_cast<int>(jpov::sp_ui::kSpUiDemoWidth * opt.scale);
    cfg.height = static_cast<int>(jpov::sp_ui::kSpUiDemoHeight * opt.scale);
    cfg.target_fps = 30;
    // 显式声明字体（路径相对 exe，由 build 脚本同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };

    SpUiDemoApp app(cfg);
    app.Init();
    app.InstallTextMeasure();
    app.SetPlateOptions(opt);
    app.SetUiScale(opt.scale);

    // 材质：平铺必须 GL_REPEAT；**不开 mipmap** —— UI 是 1:1 像素，而 fract 平铺在
    // 接缝处导数不连续，自动选 LOD 会挑到最粗 mip 并在接缝上糊一条线
    // （见 tools/jpov/known_issue.md §8.2）。
    if (!opt.no_texture) {
        const std::string tex_path = ResolveMarbleTexture();
        CHECK(!tex_path.empty())
            << "找不到大理石 flipbook 纹理（产物目录/textures 或仓库 assets/sp_ui/marble）";
        jpov::TextureOptions tex_opt;
        tex_opt.repeat = true;
        tex_opt.mipmap = false;
        app.SetMarbleTexture(app.RegisterTexture(tex_path, tex_opt));
        LOG(INFO) << "材质：" << tex_path;
    }

    if (!capture_dir.empty()) {
        // headless：每个时间点一张。时间轴固定 ⇒ 同一命令产出同一张图。
        const std::string cmd = "mkdir -p '" + capture_dir + "'";
        CHECK_EQ(std::system(cmd.c_str()), 0) << "创建输出目录失败：" << capture_dir;
        jpov::WindowInfo winfo;
        winfo.width = static_cast<int>(jpov::sp_ui::kSpUiDemoWidth * opt.scale);
        winfo.height = static_cast<int>(jpov::sp_ui::kSpUiDemoHeight * opt.scale);
        jpov::InputSnapshot input{};
        for (size_t i = 0; i < times.size(); ++i) {
            app.SetTimeSeconds(times[i]);
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/sp_ui_t%03zu_t%.2fs.png",
                          capture_dir.c_str(), i, times[i]);
            app.RunOnce(input, winfo, path);
            LOG(INFO) << "capture: " << path;
        }
    } else {
        // 交互：材质时间自走（面板上的「时间驱动」复选框控制启停）。
        app.SetAutoAdvance(true);
        app.Run();
    }

    app.Finalize();
    return 0;
}

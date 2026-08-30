// JPOV glTF 交互查看器 — 主程序
//
// 一个附属于 JPOV 的小工具：加载一个 glTF 模型到场景中，y-up 风格，
// 地平面 300×300 米高粗糙灰色 quad。光照用 DaySkyCommand（由 sky 推导平行光
// + 全局 Ambient），交互窗口底部居中 5 个滑条实时调节：太阳角度、平行光/环境
// 光基准强度、地面高度、模型缩放。
//
// 相机默认目标 (0,0,0)，初始距离 R 按模型包围盒自适应（加载后 FitRadius 计算）。
// 右键 drag 转视角，滚轮 zoom（R 缩放）。
// 产物 ELF 的第一个参数为被加载的 glTF 路径（相对/绝对均可，fallback 到
// 项目内 pliers.gltf 便于快速演示）。
//
// 架构（task #2 文档 jpov_gltf_viewer_arch.md）：
//   交互与 --four_views 拍照共用同一个 GltfViewerApp::OneIteration 渲染体，
//   只由 ViewConfig 的驱动方式不同；光照方面交互用滑条版 MakeLighting()，
//   拍照用固定 MakeNoonLighting()（headless 截图不带 UI 面板）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_gltf_viewer -- /path/to/model.gltf
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_gltf_viewer.sh
//   → output/jpov_gltf_viewer/jpov_gltf_viewer <gltf 路径>

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/demo/view_config.h"

namespace {

// 渲染/窗口分辨率（需求定稿：1280×720，不可 resize）。
constexpr int kViewerWidth  = 1280;
constexpr int kViewerHeight = 720;

// 项目内可用的演示 glTF（若命令行未指定路径时的 fallback，便于快速跑通）。
std::string DefaultGltfPath() {
    // 运行时工作目录在仓库顶层时可用；测试/演示通常显式传路径。
    return "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
}

// UI 文本默认字体 = CJK（滑条标签显中文）。拉丁字母回退由渲染层自动处理。
// 与 jpov_ui_demo 同款：JPOV 不提供隐式默认字体，cfg.fonts 显式声明，
// UiTheme::font_alias 指定 UI 文本用 CJK。
static const char* const kFontAlias = jpov::kFontBuiltinCJK;

class GltfViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // 场景资源（Init() 后填充一次，OneIteration 里只读，不重复构造/上传）。
    jpov::GltfObject gltf_;            // 被查看的模型
    uint32_t ground_mesh_ = 0;         // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;     // 高粗糙灰色地面材质
    jpov_viewer::ViewConfig view_;     // 当前视角（交互由 ApplyInput 改；four_views 赋固定角）

    // 光照滑条状态（跨帧持有，窗口下方 5 个滑条实时调节）。
    // 初始值 = 正午基准（θ=0 天顶直射 / sun 3.0 / ambient 0.3），与 MakeNoonLighting
    // 的固定正午语义一致（只是这里把太阳方向也纳入调节，θ=0 ⇒ 正午最亮）。
    float theta_  = 0.0f;              // 太阳方向参数 θ（弧度，[0, π]）
    float sun_base_  = 3.0f;           // 平行光正午基准强度（同 LIGHT_INTENSITY.md 的 3.0）
    float ambient_base_ = 0.3f;        // 环境光正午基准强度（同当前 0.3）
    float ground_y_ = -3.0f;           // 地面高度（滑条 [-3,+3]，实时调节看物体落地产影）
    float ground_y_prev_ = -3.0f;      // 上一帧地面高度（检测变化才 UpdateMesh）
    float model_scale_ = 1.0f;         // 模型整体缩放（滑条 [0.1, 20]，先缩放再旋转平移）
    jpov::Ui ui_;                      // 跨帧持有（滑条拖动态内部记忆）

    // 运行模式与帧率（main 设置）：
    bool interactive_ = true;          // 交互窗口模式（true）或 --four_views 拍照（false）
    int cfg_target_fps_ = 60;          // 帧率（Ui::Begin 的 frame_dt_ms 计时用）

    // 供 main 在 Init() 后安装文本测量回调（ui_ 私有，经此公开入口设置）。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&GltfViewerApp::ViewerTextWidth, this);
    }

    // 把 Ui 的文本测量回调接到本应用的 JPOV::MeasureTextWidth（真实字体进宽），
    // 使滑条数值文本绘制用真实字体宽度（UI 内部用行高/居中，不依赖此宽度，
    // 但保持与 jpov_ui_demo 同款接线以避免后续 InputText 类控件的宽度分叉）。
    // alias 空串 = 首个注册字体，与 demo 第一个注册字体一致。
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        GltfViewerApp* app = static_cast<GltfViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // --four_views 拍照专用标志（arch §4 note #1 认可的 headless 区分手段）。
    // 交互与拍照共用同一目标点 (0,0,0)（相机 lookAt 原点），无分叉，
    // 因此 headless 专用标志不再需要（早期曾因交互看向 (0,1,0) 而需要区分）。

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        // 渲染分辨率 = 窗口分辨率 1280×720。
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 更新视角（右键 drag 转视角 + 滚轮 zoom）。
        // four_views 拍照模式不走到这里（由外部赋固定 view_）。
        float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
        if (input.right.IsDrag()) {
            dx = input.mouse_dx;
            dy = input.mouse_dy;
        }
        if (input.scroll_delta != 0.0f) {
            scroll = input.scroll_delta;
        }
        jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                static_cast<int>(winfo.width),
                                static_cast<int>(winfo.height));

        // ── 相机：由 ViewConfig 推导 ──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();  // (0,0,0)
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;  // 场景 R 最大 300，far 足够

        // ── 光照：由 5 个滑条实时调节（sky 推导平行光 + 全局 Ambient）──
        const jpov_viewer::NoonLighting light =
            jpov_viewer::MakeLighting(theta_, sun_base_, ambient_base_);
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 被加载的 glTF ──
        // 地面高度可调：ground_y_ 变化时原地重建 quad（UpdateMesh，VBO 布局不变）
        // 让“物体落在地面上、影子投到地面上”随滑条实时变化（仅交互;
        // four_views 不动 ground_y_，保持默认 -3）。
        if (ground_y_ != ground_y_prev_) {
            UpdateMesh(ground_mesh_, jpov_viewer::MakeGroundQuad(ground_y_));
            ground_y_prev_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(gltf_, /*center*/ {0.0f, 0.0f, 0.0f},
                             /*up*/ {0.0f, 1.0f, 0.0f},
                             /*front*/ {0.0f, 0.0f, 1.0f},
                             /*picking_id*/ 0, /*highlight*/ false,
                             /*scale*/ model_scale_);

        // ── 光照调节面板：窗口底部居中，5 个滑条（各约半屏宽）──
        // 仅交互窗口模式绘制；--four_views 拍照是给 AI 自查用的 headless 截图，
        // 不带 UI 面板（保持截图即纯 3D 场景，与原有行为一致）。
        if (interactive_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 光照调节面板布局与绘制（即时模式）。
    // 5 个滑条竖排，位于窗口底部居中：
    //   1) 太阳角度 θ（弧度，[0, π]，太阳方向 (−sinθ, −cosθ, 0)）
    //   2) 平行光正午基准强度（[1, 10]，默认 3.0）
    //   3) 环境光正午基准强度（[0.1, 1.0]，默认 0.3）
    //   4) 地面高度 y（[-3, +3]，默认 -3，实时更新地面 quad）
    //   5) 模型整体缩放（[0.1, 20]，默认 1.0，先缩放再旋转平移）
    // 每个滑条宽度 = 半屏宽（kSliderWidth），水平居中；`label: value` 文本
    // 由 SliderFloat 画在滑条 box 中央（复用 Text 居中语义，见 ui.cc）。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kFontAlias;
        const float frame_dt_ms = 1000.0f / static_cast<float>(cfg_target_fps_);
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kSliderWidth = 0.5f * w;   // 半屏宽
        const float kRowH    = 30.0f;
        const float kSpacing = 12.0f;
        const float kBottom  = 20.0f;
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom - (5.0f * kRowH + 4.0f * kSpacing);

        // θ 用 2 位小数（弧度，直接影响太阳方向，精度值得看）。
        ui_.SliderFloat("太阳角度 θ", &theta_,
                        jpov::UiRect{{left, top}, {kSliderWidth, kRowH}},
                        0.0f, static_cast<float>(M_PI), /*decimal_places*/2);
        ui_.SliderFloat("平行光强度", &sun_base_,
                        jpov::UiRect{{left, top + (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        1.0f, 10.0f, /*decimal_places*/1);
        ui_.SliderFloat("环境光强度", &ambient_base_,
                        jpov::UiRect{{left, top + 2.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        0.1f, 1.0f, /*decimal_places*/2);
        // 地面高度（米）：[-3,+3]，实时看物体落地面/阴影落地面。
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, top + 3.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        // 模型整体缩放（[0.1,20] 默认 1.0，先缩放再旋转平移；验证小物体阴影/轮廓是否尺寸所致）。
        ui_.SliderFloat("模型缩放", &model_scale_,
                        jpov::UiRect{{left, top + 4.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        0.1f, 20.0f, /*decimal_places*/1);
    }

    // 滑条字号（px）。
    static constexpr float kSliderFontSize = 16.0f;
};

// 是否有 --four_views 标志（AI 自查拍照模式）。
bool HasFourViewsFlag(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--four_views") return true;
    }
    return false;
}

// 从命令行取 glTF 路径：第一个非 -- 前缀的参数（相对/绝对均可）。
// 与 --four_views 标志互不干扰（该标志排在任意位置均可）。
std::string FirstNonFlagArg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).rfind("--", 0) != 0) return argv[i];
    }
    return "";
}

// 度数 → 弧度（ViewConfig 以弧度存储）。
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// --four_views 的 4 个固定拍照角度 (phi, theta)，单位度（需求定稿）。
struct FourView {
    const char* name;   // 输出后缀（front/up/left/perspective）
    double phi_deg;
    double theta_deg;
};
constexpr FourView kFourViews[] = {
    {"front",       0.0,   0.0},   // (φ,θ)=(0,0)
    {"up",         90.0,   0.0},   // (φ,θ)=(90,0)
    {"left",        0.0,  90.0},   // (φ,θ)=(0,90)
    {"perspective",45.0,  45.0},   // (φ,θ)=(45,45)
};

// --four_views 拍照模式：headless 渲染 4 个角度，各输出一张 PNG 到
// glTF 所在目录，文件名 `<gltf_basename>_<view>.png`。不弹窗。
// 交互模式与拍照模式走同一条 OneIteration + 同一份 MakeNoonLighting()
// （zero 分叉，架构 doc），因此这里只需要逐个设置 view_ 后 RunOnce 截图。
void RunFourViews(GltfViewerApp* app, const std::string& gltf_path) {
    CHECK_NOTNULL(app);

    // 目标目录 = glTF 所在目录；输出文件名 = <模型basename>_<view>.png。
    // （不含扩展名的模型名，如 pliers → pliers_front.png）
    const size_t slash = gltf_path.find_last_of("/\\");
    const std::string dir     = (slash == std::string::npos)
                                    ? "." : gltf_path.substr(0, slash);
    const std::string full    = (slash == std::string::npos)
                                    ? gltf_path : gltf_path.substr(slash + 1);
    const size_t dot          = full.find_last_of('.');
    const std::string base    = (dot == std::string::npos)
                                    ? full : full.substr(0, dot);

    jpov::WindowInfo winfo;
    winfo.width  = kViewerWidth;
    winfo.height = kViewerHeight;

    for (const FourView& fv : kFourViews) {
        // 赋固定角度（量纲约定：ViewConfig 存弧度）。
        // R 保持 main 里按模型包围盒算好的初始距离（模型自适应），不在此重置：
        // 4 个角度都应沿用同一“看清全貌”的取景距离，才能框住模型。
        app->view_.phi   = fv.phi_deg * kDegToRad;
        app->view_.theta = fv.theta_deg * kDegToRad;

        const std::string out = dir + "/" + base + "_" + fv.name + ".png";
        jpov::InputSnapshot input{};   // 无交互输入（固定角度拍照）
        app->RunOnce(input, winfo, out.c_str());
        LOG(INFO) << "--four_views 已渲染: " << out;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool four_views = HasFourViewsFlag(argc, argv);

    // glTF 路径 = 第一个非 -- 前缀参数（相对/绝对均可）。
    std::string gltf_path = FirstNonFlagArg(argc, argv);
    if (gltf_path.empty()) {
        gltf_path = DefaultGltfPath();
        LOG(WARNING) << "未提供 glTF 路径，使用演示模型: " << gltf_path;
    }

    // ── 配置：1280×720、不可 resize、60fps。──
    // four_views 拍照模式 → headless（无可见窗口，AI 自查不弹窗）。
    JPOV::Config cfg;
    cfg.title = "JPOV — glTF 交互查看器";
    cfg.width  = kViewerWidth;
    cfg.height = kViewerHeight;
    cfg.resizable = false;          // 需求：窗口不可 resize
    cfg.target_fps = 60;            // 需求：60 帧
    cfg.headless  = four_views;     // 需求：--four_views 走 headless，不弹窗
    // 显式声明字体：CJK 显中文滑条标签，Latin 做拉丁回退（同 jpov_ui_demo）。
    // 路径为相对 exe 的字体目录（从 output/jpov_gltf_viewer/ 运行，由
    // build_jpov_gltf_viewer.sh 同构拷贝 fonts/）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    GltfViewerApp app(cfg);
    // 交互/拍照模式标志：拍照（headless 截图）不带 UI 面板。
    app.interactive_ = !four_views;
    // 帧率计时（滑条键盘 hold 用）：取 cfg.target_fps。
    app.cfg_target_fps_ = cfg.target_fps;
    app.Init();
    // 注入真实字体文本宽度测量（供 UI 内部用，与 jpov_ui_demo 同款接线）。
    app.InstallTextMeasure();

    // 加载 glTF 到中心；失败（empty）→ LOG(FATAL) 带清晰信息，不静默。
    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty())
        << "LoadGltf 失败或模型为空: " << gltf_path
        << "（请确认路径存在且为合法 .gltf/.glb）";

    // 场景静态资源只建一次（不在 OneIteration 里重复构造/上传）。
    // 光照不在此预建：交互版由 OneIteration 里的 5 个滑条实时构造
    // （θ/平行光/环境光基准强度 + 地面高度 + 模型缩放），见 DrawLightPanel。
    app.ground_mat_ = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad());

    // 初始视角：目标点 (0,0,0)；R 按模型包围盒自适应（模型大小变化 → 初始
    // 距离随之变化，总能一眼框住全貌）。退化（包围盒不可用）时用 DefaultView 默认。
    app.view_ = jpov_viewer::DefaultView();
    if (app.gltf_.bounds_valid) {
        app.view_.R = jpov_viewer::ViewConfig::FitRadius(
            app.gltf_.bounds_min, app.gltf_.bounds_max, /*fov_deg*/ 60.0);
        LOG(INFO) << "模型包围盒 [" << app.gltf_.bounds_min[0] << ","
                  << app.gltf_.bounds_min[1] << "," << app.gltf_.bounds_min[2]
                  << "] ~ [" << app.gltf_.bounds_max[0] << ","
                  << app.gltf_.bounds_max[1] << "," << app.gltf_.bounds_max[2]
                  << "]，初始 R=" << app.view_.R;
    }

    if (four_views) {
        // AI 自查模式：headless 渲染 4 个角度，输出到模型同级目录后退出。
        RunFourViews(&app, gltf_path);
    } else {
        // 交互窗口事件循环（阻塞）。
        app.Run();
    }
    app.Finalize();
    return 0;
}

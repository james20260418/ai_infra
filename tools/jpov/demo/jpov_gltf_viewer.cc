// JPOV glTF 交互查看器 — 主程序
//
// 一个附属于 JPOV 的小工具：加载一个 glTF 模型到场景中，y-up 风格，
// 地平面 300×300 米高粗糙灰色 quad，光照用 DaySkyCommand 正午配置
// （同 sun_path 测试：由 sky 推导平行光 + 全局 Ambient，太阳方向 (0,-1,-1)）。
//
// 相机默认 (1,1,1)→(0,1,0)。右键 drag 转视角，滚轮 zoom（R 缩放）。
// 产物 ELF 的第一个参数为被加载的 glTF 路径（相对/绝对均可，fallback 到
// 项目内 pliers.gltf 便于快速演示）。
//
// 架构（task #2 文档 jpov_gltf_viewer_arch.md）：
//   交互与（后续 --four_views 拍照）共用同一个 GltfViewerApp::OneIteration
//   渲染体 + 同一份 MakeNoonLighting()，只由 ViewConfig 的驱动方式不同；
//   保证 zero 分叉、截图即所见。
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

class GltfViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // 场景资源（Init() 后填充一次，OneIteration 里只读，不重复构造/上传）。
    jpov::GltfObject gltf_;            // 被查看的模型
    uint32_t ground_mesh_ = 0;         // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;     // 高粗糙灰色地面材质
    jpov_viewer::ViewConfig view_;     // 当前视角（交互由 ApplyInput 改；four_views 赋固定角）
    jpov_viewer::NoonLighting noon_;   // 正午光照（sky/sun/ambient 一次构造）

    // --four_views 拍照专用标志（arch §4 note #1 认可的 headless 区分手段）。
    // 交互模式看向经典目标点 (0,1,0)；而拍照要“看到模型”，模型画在原点 (0,0,0)
    // （贴地）。若拍照也看向 (0,1,0)，phi=0 的 front/left 会从 y=1 平视、
    // 从小模型头顶看过去，模型落在视锥下缘之外拍不到（实测 bug，task#7 修复）。
    // 因此拍照模式把相机目标点改到模型原点，让 4 张图都能框住模型。
    bool headless_shot_ = false;

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
        cmds->camera.target   = headless_shot_
                                    ? jpov::Vec3f{0.0f, 0.0f, 0.0f}  // 拍照看模型
                                    : jpov_viewer::ViewConfig::Target();  // 交互看 (0,1,0)
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;  // 场景 R 最大 300，far 足够

        // ── 光照：正午（sky 推导平行光 + 全局 Ambient）──
        cmds->sky = noon_.sky;
        cmds->sun = noon_.sun;
        cmds->ambient = noon_.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 被加载的 glTF ──
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(gltf_, /*center*/ {0.0f, 0.0f, 0.0f},
                             /*up*/ {0.0f, 1.0f, 0.0f},
                             /*front*/ {0.0f, 0.0f, 1.0f});
    }
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

    // 拍照模式：相机目标点改到模型原点，让 4 个角度都能框住模型
    // （交互默认目标 (0,1,0) 在 phi=0 时从 y=1 平视，小模型落在视锥下缘外，
    //   front/left 会拍不到——见架构 §4 note#1 与 task#7 自测 bug）。
    app->headless_shot_ = true;

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
        app->view_.phi   = fv.phi_deg * kDegToRad;
        app->view_.theta = fv.theta_deg * kDegToRad;
        app->view_.R     = jpov_viewer::DefaultView().R;  // 与交互默认同距

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

    GltfViewerApp app(cfg);
    app.Init();

    // 加载 glTF 到中心；失败（empty）→ LOG(FATAL) 带清晰信息，不静默。
    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty())
        << "LoadGltf 失败或模型为空: " << gltf_path
        << "（请确认路径存在且为合法 .gltf/.glb）";

    // 场景静态资源只建一次（不在 OneIteration 里重复构造/上传）。
    app.ground_mat_ = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad());
    app.noon_ = jpov_viewer::MakeNoonLighting();
    app.view_ = jpov_viewer::DefaultView();  // (1,1,1)→(0,1,0)

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

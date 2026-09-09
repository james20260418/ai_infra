// JPOV JPOV 模型查看器 — 主程序（装配 + 模式分发）
//
// 一个附属于 JPOV 的小工具：加载一个 glTF 模型到场景中，y-up，地平面 300×300 米
// 高粗糙灰色 quad，光照用 DaySkyCommand（由 sky 推导平行光 + 全局 Ambient），
// 窗口底部居中 5 个滑条实时调节（仰角 / 浊度 / 季节色温 / 地面高度 / 模型缩放）。
//
// 架构（重构后，docs/jpov_model_viewer_arch.md）：本主程序只做三件事——
//   1. 解析 CLI → 类型化 RunMode（见下），不解释模式内部结构；
//   2. 装配 ViewerApp（场景 + 字体 + 相机自适应），见 viewer_app.h；
//   3. 按模式分发：
//        interactive → app.Run()（事件循环）
//        four_views / round_video → 构造 CaptureSpec 交给 viewer_capture.h 统一 driver。
//   渲染核心、UI、拍摄编排、纯文件工具分别归属 viewer_app.h / viewer_capture.h /
//   viewer_output.h，本文件不再膨胀成承载多种身份的“大杂烩”。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_model_viewer -- /path/to/model.gltf
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_model_viewer.sh
//   → output/jpov_model_viewer/jpov_model_viewer <gltf 路径>

#include <cstdlib>  // atof / atoi
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/demo/viewer_capture.h"

namespace {

// 运行模式（类型化，取代历史两姊妹裸 bool four_views/round_video 的手工互斥。
// 未来加新 headless 子命令（round_gif / sun_path…）只需扩枚举 + 一个分支，不会
// 让 main 的 if-else 布尔丛林失控）。
enum class RunMode { kInteractive, kFourViews, kRoundVideo, kInvalid };

// 项目内可用的演示 glTF（若命令行未指定路径时的 fallback，便于快速跑通）。
std::string DefaultGltfPath() {
    // 运行时工作目录在仓库顶层时可用；测试/演示通常显式传路径。
    return "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
}

// 命令行解析结果。产出=解析 + 返回 (run_mode, spec-or-null, gltf_path)。
// 交互模式 spec 无意义（kInteractive 时仅 gltf_path 有效）。
struct CliParsed {
    RunMode mode = RunMode::kInteractive;
    // 仅拍摄模式填充（仍统一传 gltf/output_dir；四视图与 round 具体参数字段分开，
    // 避免一个 struct 塞满两套无用参数——子字段按 kind 有意义）。
    struct {
        std::string output_dir;
        double phi_deg = 45.0;   // round: 相机仰角（俯视 45°）
        int    frames  = 60;     // round: 总帧数
        int    fps     = 10;     // round: 视频帧率（60 帧@10fps = 6s）
    } capture;
    std::string gltf_path;
};

// 解析 CLI。遍历 argv[1..]：纯标志（--four_views / --round_video）与带值标志
// （--output_dir / --phi_deg / --frames / --fps）可任意位置/顺序；第一个非 "--"
// 前缀参数 = glTF 路径（--output_dir 会吞掉紧跟值，故模型路径取剩余首个非标志参）。
// 未知标志 → WARNING 忽略（不崩溃）。互斥模式同时给 → 后者优先（kInvalid 不用）。
CliParsed ParseCli(int argc, char** argv) {
    CliParsed p;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--four_views") {
            p.mode = RunMode::kFourViews;
        } else if (arg == "--round_video") {
            p.mode = RunMode::kRoundVideo;
        } else if (arg == "--output_dir") {
            if (i + 1 < argc) p.capture.output_dir = argv[++i];
            else LOG(WARNING) << "--output_dir 缺少目录参数，忽略";
        } else if (arg == "--phi_deg") {
            if (i + 1 < argc) p.capture.phi_deg = std::atof(argv[++i]);
            else LOG(WARNING) << "--phi_deg 缺少数值，忽略";
        } else if (arg == "--frames") {
            if (i + 1 < argc) p.capture.frames = std::atoi(argv[++i]);
            else LOG(WARNING) << "--frames 缺少数值，忽略";
        } else if (arg == "--fps") {
            if (i + 1 < argc) p.capture.fps = std::atoi(argv[++i]);
            else LOG(WARNING) << "--fps 缺少数值，忽略";
        } else if (arg.rfind("--", 0) == 0) {
            LOG(WARNING) << "未知参数: " << arg << "; 已忽略";
        } else if (p.gltf_path.empty()) {
            p.gltf_path = arg;
        } else {
            LOG(WARNING) << "多余位置参数: " << arg << "; 已忽略";
        }
    }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    const CliParsed p = ParseCli(argc, argv);
    const bool capture = (p.mode == RunMode::kFourViews ||
                          p.mode == RunMode::kRoundVideo);

    // glTF 路径 = 第一个非 "--" 前缀参数；缺省退回演示模型。
    std::string gltf_path = p.gltf_path;
    if (gltf_path.empty()) {
        gltf_path = DefaultGltfPath();
        LOG(WARNING) << "未提供 glTF 路径，使用演示模型: " << gltf_path;
    }

    // ── 配置：1280×720 不可 resize、60fps。拍摄模式 headless（无可见窗口）。──
    JPOV::Config cfg;
    cfg.title = "JPOV — JPOV 模型查看器";
    cfg.width  = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.resizable = false;             // 需求：窗口不可 resize
    cfg.target_fps = static_cast<int>(jpov_viewer::kViewerFps);
    cfg.headless   = capture;          // 拍摄 headless 出图不弹窗
    // 显式声明字体：CJK 显中文滑条标签，Latin 做拉丁回退（路径相对 exe 的 fonts/，
    // build_jpov_model_viewer.sh 同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_viewer::ViewerApp app(cfg);
    app.SetShowPanel(!capture);        // 交互画面板；headless 拍摄不画（纯 3D 截图）
    app.Init();
    app.InstallTextMeasure();          // 交互 UI + UI 文本测量（拍摄装了无副作用）

    // 加载 glTF 到中心；失败 → LOG(FATAL) 带清晰信息，不静默。
    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty())
        << "LoadGltf 失败或模型为空: " << gltf_path
        << "（请确认路径存在且为合法 .gltf/.glb）";

    // 场景静态资源只建一次（不在 OneIteration 里重复构造/上传）。
    app.ground_mat_  = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad());

    // 初始视角：目标原点、R 按模型包围盒自适应（退化时退回 DefaultView）。
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

    // ── 模式分发 ──
    if (p.mode == RunMode::kFourViews || p.mode == RunMode::kRoundVideo) {
        // 把 CLI 拍平成一张 CaptureSpec 交给统一 driver（viewer_capture.h）。
        jpov_viewer::CaptureSpec spec;
        spec.kind        = (p.mode == RunMode::kFourViews)
                               ? jpov_viewer::CaptureKind::kFourViews
                               : jpov_viewer::CaptureKind::kRoundVideo;
        spec.gltf_path   = gltf_path;
        spec.output_dir  = p.capture.output_dir;
        spec.phi_deg     = p.capture.phi_deg;
        spec.num_frames  = p.capture.frames;
        spec.fps         = p.capture.fps;
        if (!jpov_viewer::RunCapture(&app, spec)) {
            LOG(FATAL) << "拍摄失败（详见上方 ERROR 日志）";
        }
    } else {
        // 交互窗口事件循环（阻塞）。
        app.Run();
    }

    app.Finalize();
    return 0;
}

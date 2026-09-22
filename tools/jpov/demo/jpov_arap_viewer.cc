// JPOV ARAP 查看器 — 主程序（装配 + 模式分发）
//
// 与 jpov_model_viewer 并列的第二个交互式查看器。相同点：加载一个 glTF 模型、
// y-up 球面角相机、SkyCommand 推导光照、300×300 灰色地面。不同点：多一套**软体
// 动力学仿真**（ARAP 弹性 + PBD 地面碰撞 + 重力 + 阻尼），可把网格从高处摔到地面，
// 观察其「充气城堡」式回弹——交互面板多出「动力学启停 / 重置 mesh」两个按钮，
// 以及一个「地面高度」滑条（实时改摔落目标面）。
//
// 结构（与 model viewer 同款「薄 main + 厚 App」分工）：
//   demo/arap_sim.{h,cc}        —— 纯 CPU 软体仿真（焊接 + PBD 三类约束），可单测
//   interface/mesh_geometry.h   —— 法线/切线 CPU 重算（自推、不焊接），可单测
//   demo/arap_viewer_app.h      —— 渲染核心 App（仿真宿主 + 交互面板）
//   本文件                      —— CLI 解析 + 装配 + 交互/headless 分发
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_arap_viewer -- /path/to/model.glb
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_arap_viewer.sh
//   → output/jpov_arap_viewer/jpov_arap_viewer <glb 路径>

#include <cmath>
#include <cstdio>
#include <cstdlib>  // atoi / atof
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/arap_viewer_app.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_output.h"

namespace {

// 运行模式：交互窗口 / headless 摔落序列出图。
enum class RunMode {
    kInteractive,
    kHeadlessDrop,
    kUiShot,   // headless 单帧 + 显示交互面板（UI 自检，不推进动力学）
};

// 命令行解析结果。
struct CliParsed {
    RunMode mode = RunMode::kInteractive;
    std::string gltf_path;
    bool use_box = false;        // 用内置方块代替资产文件（未给路径时也自动启用）
    std::string output_dir;      // 仅 headless：产物目录（空 = glTF 同级）
    int frames = 90;             // 仅 headless：推进的总帧数
    int every = 10;              // 仅 headless：每 N 帧落盘一张（第 0 帧恒落）
};

// 内置方块模式的产物文件名前缀（无资产文件时用于命名输出）。
constexpr const char* kBuiltinBoxBase = "builtin_box";

// 解析 CLI：纯标志与带值标志可任意顺序；首个非 "--" 参数 = glTF 路径。
// 未知标志 → WARNING 忽略（不崩溃），与 model viewer 同款宽松策略。
CliParsed ParseCli(int argc, char** argv) {
    CliParsed p;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless_drop") {
            p.mode = RunMode::kHeadlessDrop;
        } else if (arg == "--ui_shot") {
            p.mode = RunMode::kUiShot;
        } else if (arg == "--box") {
            p.use_box = true;
        } else if (arg == "--output_dir") {
            if (i + 1 < argc) {
                p.output_dir = argv[++i];
            } else {
                LOG(WARNING) << "--output_dir 缺少目录参数，忽略";
            }
        } else if (arg == "--frames") {
            if (i + 1 < argc) {
                p.frames = std::atoi(argv[++i]);
            } else {
                LOG(WARNING) << "--frames 缺少数值，忽略";
            }
        } else if (arg == "--every") {
            if (i + 1 < argc) {
                p.every = std::atoi(argv[++i]);
            } else {
                LOG(WARNING) << "--every 缺少数值，忽略";
            }
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

// headless：从 bind pose 起推进 frames 帧，每 every 帧落盘一张 PNG（第 0 帧恒落）。
//
// 每帧都走 App::OneIteration（与交互窗口同一条渲染体，零分叉）；区别只是
// 面板关闭、输入为空、动力学强制开启。
bool RunHeadlessDrop(jpov_arap_viewer::ArapViewerApp* app,
                     const CliParsed& cli) {
    CHECK_NOTNULL(app);
    CHECK_GT(cli.frames, 0) << "--frames 必须 > 0";
    CHECK_GT(cli.every, 0) << "--every 必须 > 0";

    // 输出 base：有资产文件时用资产名；内置方块时用固定名（无目录可推）。
    std::string out_dir;
    std::string base;
    if (cli.use_box) {
        out_dir = cli.output_dir.empty() ? "." : cli.output_dir;
        base = kBuiltinBoxBase;
    } else {
        const jpov_viewer::OutputTarget target =
            jpov_viewer::ResolveOutputTarget(cli.gltf_path, cli.output_dir);
        out_dir = target.dir;
        base = target.base;
    }
    if (!jpov_viewer::EnsureOutputDir(out_dir)) {
        LOG(ERROR) << "无法创建输出目录: " << out_dir;
        return false;
    }

    app->SetShowPanel(false);       // 纯 3D 截图（不画面板）
    app->SetDynamicsRunning(true);  // 从 bind pose 起自由落体

    const jpov::InputSnapshot input{};              // 空输入（无用户）
    const jpov::WindowInfo winfo{jpov_arap_viewer::kViewerWidth,
                                jpov_arap_viewer::kViewerHeight};
    // 非落盘帧写到一个会被覆盖的临时文件，避免每次 RunOnce 生成无意义产物。
    const std::string scratch = "/tmp/jpov_arap_scratch.png";

    int saved = 0;
    for (int f = 0; f < cli.frames; ++f) {
        const bool keep = (f % cli.every == 0) || (f == cli.frames - 1);
        std::string path = scratch;
        if (keep) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s_drop_%03d.png", base.c_str(), f);
            path = out_dir + "/" + name;
        }
        app->RunOnce(input, winfo, path.c_str());
        if (keep) {
            ++saved;
            // 诊断：打印当前几何包围盒 + 形状残差，便于判断“模型是否跑出画面/炸开”。
            // （跑出画面既可能是取景不当，也可能是仿真发散——不打数据就只能靠猜。）
            LOG(INFO) << "已出图: " << path;
            // 诊断：整体 bbox（判断是否跑出画面/瘫在地上）+ 形状残差 + 边长畸变
            // + 退化旋转计数（薄壳局部旋转是否解得出）。
            float lo[3] = {1e30f, 1e30f, 1e30f};
            float hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const auto& prim : app->primitives()) {
                for (const jpov::Vec3f& p : prim.mesh.positions) {
                    lo[0] = std::min(lo[0], p.x());
                    hi[0] = std::max(hi[0], p.x());
                    lo[1] = std::min(lo[1], p.y());
                    hi[1] = std::max(hi[1], p.y());
                    lo[2] = std::min(lo[2], p.z());
                    hi[2] = std::max(hi[2], p.z());
                }
            }
            const jpov_arap::ArapSim& sim = app->sim();
            LOG(INFO) << "  frame " << f << " bbox=[" << lo[0] << "," << lo[1]
                      << "," << lo[2] << "]~[" << hi[0] << "," << hi[1] << ","
                      << hi[2] << "] 残差=" << sim.shape_residual_rms()
                      << " 边长畸变=" << sim.edge_distortion_rms()
                      << " 弯曲=" << sim.bend_distortion_rms()
                      << " 退化旋转=" << sim.degenerate_rotation_count() << "/"
                      << sim.particle_count();
        }
    }
    LOG(INFO) << "headless 摔落序列完成: " << saved << " 张（目录 " << out_dir
              << "）";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const CliParsed cli = ParseCli(argc, argv);

    // 未提供模型路径 ⇒ 退化为内置方块（零资产依赖，最容易看清弹跳行为）。
    const bool use_box = cli.use_box || cli.gltf_path.empty();
    const std::string gltf_path = cli.gltf_path;
    if (use_box) {
        LOG(WARNING) << "使用内置方块（--box 或未提供模型路径）";
    }

    const bool headless = (cli.mode != RunMode::kInteractive);

    // ── 配置：1280×720 不可 resize，60fps；headless 不弹窗。──
    JPOV::Config cfg;
    cfg.title = "JPOV — ARAP 软体查看器";
    cfg.width = jpov_arap_viewer::kViewerWidth;
    cfg.height = jpov_arap_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = static_cast<int>(jpov_arap_viewer::kViewerFps);
    cfg.headless = headless;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };

    jpov_arap_viewer::ArapViewerApp app(cfg);
    app.Init();
    app.InstallTextMeasure();

    if (use_box) {
        CHECK(app.LoadBuiltinBox()) << "内置方块装载失败";
    } else if (!app.LoadModel(gltf_path)) {
        LOG(FATAL) << "加载模型失败: " << gltf_path
                   << "（请确认路径存在且为合法 .gltf/.glb）";
    }

    // 初始视角：默认 45° 方位、20° 仰角。取景要同时容下「模型（起于原点附近）」与
    // 「落点（地面 y = ground_y_）」：把相机目标点下移到两者中点，并按所需垂直
    // 半跨度反推 R（俯仰 60° 视锥）。只靠模型包围盒自适应会贴脸而看不到地面。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 20.0 * (M_PI / 180.0);
    float bmin[3] = {-1.0f, -1.0f, -1.0f};
    float bmax[3] = {1.0f, 1.0f, 1.0f};
    {
        bool first = true;
        for (const auto& prim : app.primitives()) {
            for (const jpov::Vec3f& p : prim.mesh.positions) {
                if (first) {
                    bmin[0] = bmax[0] = p.x();
                    bmin[1] = bmax[1] = p.y();
                    bmin[2] = bmax[2] = p.z();
                    first = false;
                    continue;
                }
                bmin[0] = std::min(bmin[0], p.x());
                bmax[0] = std::max(bmax[0], p.x());
                bmin[1] = std::min(bmin[1], p.y());
                bmax[1] = std::max(bmax[1], p.y());
                bmin[2] = std::min(bmin[2], p.z());
                bmax[2] = std::max(bmax[2], p.z());
            }
        }
    }
    // 地面自动定位：放在模型底部下方 0.5 个模型高度处（⇒ 摔落高度 = 0.5 高度）。
    // 这样任何尺度的资产都不会一开始就埋进地面（那是「首帧爆炸」的根因），
    // 滑条范围也按模型尺度自适应。
    const float model_h = std::max(1e-4f, bmax[1] - bmin[1]);
    app.ground_y_ = bmin[1] - 0.5f * model_h;
    app.ground_y_min_ = bmin[1] - 3.0f * model_h;
    app.ground_y_max_ = bmin[1] + 0.5f * model_h;

    constexpr float kMargin = 0.3f;
    const float top = bmax[1] + kMargin;
    const float bottom = app.ground_y_ - kMargin;
    app.camera_target_y_ = 0.5f * (top + bottom);
    // 物体包围球半径（确保模型自身不超框）。
    const float dx = bmax[0] - bmin[0];
    const float dy = bmax[1] - bmin[1];
    const float dz = bmax[2] - bmin[2];
    const float obj_radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    const float half_v = 0.5f * (top - bottom);
    const float need = std::max(half_v, obj_radius);
    const float half_fov = 0.5f * 60.0f * (static_cast<float>(M_PI) / 180.0f);
    app.view_.R = static_cast<double>(need / std::tan(half_fov) * 1.05f);

    LOG(INFO) << "模型包围盒 [" << bmin[0] << "," << bmin[1] << "," << bmin[2]
              << "] ~ [" << bmax[0] << "," << bmax[1] << "," << bmax[2]
              << "]，目标点 y=" << app.camera_target_y_
              << "，初始 R=" << app.view_.R;

    bool ok = true;
    if (cli.mode == RunMode::kHeadlessDrop) {
        ok = RunHeadlessDrop(&app, cli);
    } else if (cli.mode == RunMode::kUiShot) {
        // UI 自检：单帧、显示面板、不推进动力学。用于无显示器环境下检查
        // 面板布局/字体加载是否正常（交互验收前的自查）。
        CHECK(!cli.output_dir.empty()) << "--ui_shot 需要 --output_dir";
        if (!jpov_viewer::EnsureOutputDir(cli.output_dir)) {
            LOG(FATAL) << "无法创建输出目录: " << cli.output_dir;
        }
        app.SetShowPanel(true);
        app.SetDynamicsRunning(false);
        const jpov::InputSnapshot input{};
        const jpov::WindowInfo winfo{jpov_arap_viewer::kViewerWidth,
                                    jpov_arap_viewer::kViewerHeight};
        const std::string path = cli.output_dir + "/arap_viewer_ui.png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "UI 自检出图: " << path;
    } else {
        app.Run();  // 交互事件循环（阻塞）
    }

    // 先释放模型 GPU 资源（需存活 GL context），再 Finalize。
    app.ReleaseModel();
    app.Finalize();
    return ok ? 0 : 1;
}

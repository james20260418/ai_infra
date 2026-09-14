// JPOV FBX 观察器 — 主程序（装配 + 模式分发）
//
// 用法：
//   jpov_fbx_viewer <fbx 路径>                      交互窗口（需 DISPLAY/WSLg）
//   jpov_fbx_viewer <fbx 路径> --shot out.png        headless 单帧出图（AI 自查/留证）
//       可选：--time <秒>   指定动画时刻（默认 0）
//             --frame <n>  指定源帧号（优先级低于 --time；按 fbx 的 fps 换算成时刻）
//             --rest       固定 pose = identity（rest/T-pose）出图
//
// 交互操作：右键 drag 转视角、滚轮 zoom（与模型查看器同款）；底部面板两行 ——
//   一行两个控件（暂停/继续 按钮 + 固定 rest 位姿 复选框），一行状态文本（帧号/帧频/时刻）。
//
// 架构：本主程序只做三件事 —— 解析 CLI、装配 FbxViewerApp、按模式分发
// （交互 Run / headless RunOnce）。渲染与播放逻辑全在 fbx_viewer_app.h，交互与出图
// 共用同一条 OneIteration（zero 分叉）。设计说明见 docs/jpov_fbx_viewer_design.md。

#include <cstdlib>  // atof / atoi
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/fbx_viewer/fbx_viewer_app.h"

namespace {

// 命令行解析结果。
struct CliParsed {
    std::string fbx_path;      // 被观察的 FBX（第一个非 "--" 参数）
    std::string shot_path;     // 非空 = headless 单帧出图到该路径
    bool has_time = false;     // --time 是否给出
    double time_seconds = 0.0; // --time 的值（动画时刻，秒）
    bool has_frame = false;    // --frame 是否给出
    int frame_index = 0;       // --frame 的值（源帧号，按 fps 换算时刻）
    bool rest = false;         // --rest：固定 pose = identity
};

// 解析 CLI：标志可任意顺序，第一个非 "--" 参数为 FBX 路径；未知标志 WARNING 忽略。
CliParsed ParseCli(int argc, char** argv) {
    CliParsed p;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot") {
            if (i + 1 < argc) {
                p.shot_path = argv[++i];
            } else {
                LOG(WARNING) << "--shot 缺少输出路径，忽略";
            }
        } else if (arg == "--time") {
            if (i + 1 < argc) {
                p.time_seconds = std::atof(argv[++i]);
                p.has_time = true;
            } else {
                LOG(WARNING) << "--time 缺少数值，忽略";
            }
        } else if (arg == "--frame") {
            if (i + 1 < argc) {
                p.frame_index = std::atoi(argv[++i]);
                p.has_frame = true;
            } else {
                LOG(WARNING) << "--frame 缺少数值，忽略";
            }
        } else if (arg == "--rest") {
            p.rest = true;
        } else if (arg.rfind("--", 0) == 0) {
            LOG(WARNING) << "未知参数: " << arg << "；已忽略";
        } else if (p.fbx_path.empty()) {
            p.fbx_path = arg;
        } else {
            LOG(WARNING) << "多余位置参数: " << arg << "；已忽略";
        }
    }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    const CliParsed p = ParseCli(argc, argv);
    CHECK(!p.fbx_path.empty())
        << "用法: jpov_fbx_viewer <fbx 路径> [--shot out.png] [--time 秒|--frame 帧号] "
           "[--rest]";
    const bool capture = !p.shot_path.empty();
    CHECK(!(p.has_time && p.has_frame))
        << "--time 与 --frame 只能给一个（都指出的是同一件事：看哪个时刻的帧）";

    // ── 配置：1280×720 不可 resize、60fps；出图模式 headless（不弹窗）。──
    JPOV::Config cfg;
    cfg.title = "JPOV — FBX 观察器";
    cfg.width  = jpov_fbx_viewer::kViewerWidth;
    cfg.height = jpov_fbx_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = static_cast<int>(jpov_fbx_viewer::kViewerFps);
    cfg.headless = capture;
    // 显式声明字体：CJK 显中文面板标签，Latin 做拉丁回退（路径相对 exe 的 fonts/，
    // build_jpov_fbx_viewer.sh 同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_fbx_viewer::FbxViewerApp app(cfg);
    app.SetShowPanel(!capture);   // 交互画面板；headless 出图是纯 3D 截图
    app.Init();
    app.InstallTextMeasure();

    CHECK(app.LoadFbx(p.fbx_path)) << "装配失败: " << p.fbx_path;

    if (capture) {
        // headless 单帧：先把时间/模式设好，再 RunOnce（OneIteration 画的是"推进前"
        // 的时刻，故设定值即出图时刻）。
        if (p.has_time) {
            app.anim_time_seconds_ = p.time_seconds;
        } else if (p.has_frame) {
            app.anim_time_seconds_ = static_cast<double>(p.frame_index) /
                                     app.clip_.frames_per_second;
        }
        app.rest_pose_mode_ = p.rest;
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(jpov_fbx_viewer::kViewerWidth);
        winfo.height = static_cast<float>(jpov_fbx_viewer::kViewerHeight);
        const jpov::InputSnapshot input{};
        app.RunOnce(input, winfo, p.shot_path.c_str());
        LOG(INFO) << "已出图: " << p.shot_path;
    } else {
        app.Run();  // 交互事件循环（阻塞）
    }

    app.Finalize();
    return 0;
}

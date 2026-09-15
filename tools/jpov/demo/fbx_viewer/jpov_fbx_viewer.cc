// JPOV FBX 观察器 — 主程序（装配 + 模式分发）
//
// 用法：
//   jpov_fbx_viewer <fbx 路径> [glb 路径]            交互窗口（需 DISPLAY/WSLg）
//   jpov_fbx_viewer <fbx 路径> [glb 路径] --shot out.png   headless 单帧出图
//
// 可选开关（详细说明见 fbx_viewer_app.h 的 MeshSource / BlueDrive）：
//   --mesh 0|1|2   看源（红）/ 目标（蓝）/ 两者并列
//   --drive 0|1    蓝骨驱动：0=无重定向（数值直搬，对照）/ 1=QRetarget（正式重定向）
//   --time 秒 | --frame 帧号 | --rest
//       可选：--time <秒>   指定动画时刻（默认 0）
//             --frame <n>  指定源帧号（优先级低于 --time；按 fbx 的 fps 换算成时刻）
//             --rest       固定 pose = identity（rest/T-pose）出图
//             --mesh <n>   指定「mesh 来源」出图（0=仅 fbx 红 / 1=仅 glb 蓝 / 2=两者并列，
//                          默认：传了 glb 就 2，否则 0）——与面板 combo 同一套语义。
//   → 第二个位置参数（可选）是 **glb 路径**：给了它就多一个「对照组」——蓝骨 = 从该 glb
//     读出的 rest 骨架，用同一份 fbx pose **数值直搬**（不做重定向）驱动，用来肉眼验证
//     “不加重定向直接搬 lcl rotation 会不对”（见 fbx_viewer_app.h 头注 / 设计文档）。
//
// 交互操作：右键 drag 转视角、滚轮 zoom（与模型查看器同款）；**顶部**面板两行 ——
//   控件行（mesh 来源 combo / 暂停按钮 / rest 复选框）+ 状态行。
//   （面板贴顶而非贴底：下拉展开列表向下画，贴底时选项会伸出屏幕外。）
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
    std::string glb_path;      // 可选的目标骨架 glb（第二个非 "--" 参数；空 = 不启用对照组）
    std::string shot_path;     // 非空 = headless 单帧出图到该路径
    bool has_time = false;     // --time 是否给出
    double time_seconds = 0.0; // --time 的值（动画时刻，秒）
    bool has_frame = false;    // --frame 是否给出
    int frame_index = 0;       // --frame 的值（源帧号，按 fps 换算时刻）
    bool has_mesh_source = false;  // --mesh 是否给出
    int mesh_source = 0;           // --mesh 的值（0=仅 fbx / 1=仅 glb / 2=两者并列）
    bool has_blue_drive = false;   // --drive 是否给出
    int blue_drive = 0;            // --drive 的值（0=无重定向直搬 / 1=QRetarget）
    bool rest = false;         // --rest：固定 pose = identity
};

// 解析 CLI：标志可任意顺序，位置参数按序 = fbx / [glb]；未知标志 WARNING 忽略。
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
        } else if (arg == "--mesh") {
            if (i + 1 < argc) {
                p.mesh_source = std::atoi(argv[++i]);  // 与 MeshSource 枚举值同序
                p.has_mesh_source = true;
            } else {
                LOG(WARNING) << "--mesh 缺少数值，忽略";
            }
        } else if (arg == "--drive") {
            if (i + 1 < argc) {
                p.blue_drive = std::atoi(argv[++i]);  // 与 BlueDrive 枚举值同序
                p.has_blue_drive = true;
            } else {
                LOG(WARNING) << "--drive 缺少数值，忽略";
            }
        } else if (arg == "--rest") {
            p.rest = true;
        } else if (arg.rfind("--", 0) == 0) {
            LOG(WARNING) << "未知参数: " << arg << "；已忽略";
        } else if (p.fbx_path.empty()) {
            p.fbx_path = arg;
        } else if (p.glb_path.empty()) {
            p.glb_path = arg;   // 第二个位置参数 = 可选的目标骨架 glb
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
        << "用法: jpov_fbx_viewer <fbx 路径> [glb 路径] [--shot out.png] "
           "[--time 秒|--frame 帧号] [--rest] [--mesh 0|1|2] [--drive 0|1]";
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
    if (!p.glb_path.empty()) {
        // 可选对照组：glb 的 rest 骨架（蓝）+ 同一份 fbx pose 数值直搬（无重定向）。
        CHECK(app.LoadGlbSkeleton(p.glb_path)) << "glb 目标骨架装配失败: " << p.glb_path;
    }

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
        if (p.has_mesh_source) {
            CHECK_GE(p.mesh_source, 0);
            CHECK_LT(p.mesh_source, jpov_fbx_viewer::kMeshSourceItemCount)
                << "--mesh 取值应为 0.." << (jpov_fbx_viewer::kMeshSourceItemCount - 1);
            CHECK(p.mesh_source == static_cast<int>(jpov_fbx_viewer::MeshSource::kFbxOnly) ||
                  app.has_glb_)
                << "--mesh 选了 glb（1/2）但未给 glb 路径";
            app.mesh_source_ =
                static_cast<jpov_fbx_viewer::MeshSource>(p.mesh_source);
        }
        if (p.has_blue_drive) {
            CHECK_GE(p.blue_drive, 0);
            CHECK_LT(p.blue_drive, jpov_fbx_viewer::kBlueDriveItemCount)
                << "--drive 取值应为 0.." << (jpov_fbx_viewer::kBlueDriveItemCount - 1);
            CHECK(app.has_glb_) << "--drive 需要 glb 目标骨架（第二位置参数）";
            app.blue_drive_ =
                static_cast<jpov_fbx_viewer::BlueDrive>(p.blue_drive);
        }
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

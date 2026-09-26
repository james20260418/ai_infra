// JPOV FBX 观察器 — 主程序（装配 + 模式分发）
//
// 用法：
//   jpov_fbx_viewer <fbx 路径> [glb 路径]            交互窗口（需 DISPLAY/WSLg）
//   jpov_fbx_viewer <fbx 路径> [glb 路径] --shot out.png   headless 单帧出图
//
// 可选开关（详细说明见 fbx_viewer_app.h 的 ViewMode）：
//   --view 0|1|2|3|4|5|6 显示/驱动：0 只看红 / 1 只看蓝(BodyRetarget) / 2 并列(BodyRetarget)
//                                / 3 只看蓝(无重定向对照) / 4 并列(无重定向对照)
//                                / 5 并列(红骨人 + 蓝**带皮网格**，GPU 双帧插值)
//                                / 6 并列(红骨人 + **3 个 instanced 蓝肉人**，验收真 instancing)
//   --time 秒 | --frame 帧号 | --rest
//       可选：--time <秒>   指定动画时刻（默认 0）
//             --frame <n>  指定源帧号（优先级低于 --time；按 fbx 的 fps 换算成时刻）
//             --rest       固定 pose = identity（rest/T-pose）出图
//             --view <n>   指定「显示/驱动」出图（0..7，见 fbx_viewer_app.h 的 ViewMode）
//                          ——与面板 combo 同一套语义。
//   → 第二个位置参数（可选）是 **glb 路径**：给了它就多一个「目标骨架（蓝）」——蓝骨 = 从该
//     glb 读出的 rest 骨架，驱动方式由 `--view`（或面板 combo）选：
//     **BodyRetarget**（正式重定向）或**数值直搬**（无重定向对照，用来肉眼验证
//     “不加重定向直接搬 lcl rotation 会不对”）。见 fbx_viewer_app.h 头注 / 设计文档。
//
// 交互操作：右键 drag 转视角、滚轮 zoom（与模型查看器同款）；**顶部**面板两行 ——
//   控件行（显示/驱动 combo / 暂停按钮 / rest 复选框）+ 状态行。
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
    bool has_view = false;         // --view 是否给出
    // --view 的值（ViewMode 下标：0 只看红 / 1 只看蓝 / 2 并列 / 3 蓝直搬 /
    //   4 并列直搬 / 5 并列带皮网格 / 6 并列 3 蓝肉人·instanced / 7 并列带皮·粗细微调）。
    int view_mode = 0;
    bool rest = false;         // --rest：固定 pose = identity
    // --thick-leg / --thick-arm：部位粗细（组 0 = 腿 / 组 1 = 手臂）系数，
    //   **交互与 headless 出图都生效**（交互时也可继续用面板滑条改）。
    //   与 ViewMode::kSkinnedThickness 的面板滑条是同一份状态。
    float thick_leg = 1.0f;
    float thick_arm = 1.0f;
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
        } else if (arg == "--view") {
            if (i + 1 < argc) {
                p.view_mode = std::atoi(argv[++i]);  // 与 ViewMode 枚举值同序
                p.has_view = true;
            } else {
                LOG(WARNING) << "--view 缺少数值，忽略";
            }
        } else if (arg == "--rest") {
            p.rest = true;
        } else if (arg == "--thick-leg") {
            if (i + 1 < argc) {
                p.thick_leg = static_cast<float>(std::atof(argv[++i]));
            } else {
                LOG(WARNING) << "--thick-leg 缺少数值，忽略";
            }
        } else if (arg == "--thick-arm") {
            if (i + 1 < argc) {
                p.thick_arm = static_cast<float>(std::atof(argv[++i]));
            } else {
                LOG(WARNING) << "--thick-arm 缺少数值，忽略";
            }
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
           "[--time 秒|--frame 帧号] [--rest] [--view 0..7] "
           "[--thick-leg 0.3~2.0] [--thick-arm 0.3~2.0]";
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
        // 可选目标骨架：glb 的 rest 骨架（蓝），驱动方式由 view_mode_ 决定。
        CHECK(app.LoadGlbSkeleton(p.glb_path)) << "glb 目标骨架装配失败: " << p.glb_path;
    }

    // 部位粗细滑条值（与面板滑条同一份状态）：交互与出图都生效。
    //   值域与面板滑条一致（kThicknessScaleMin..kThicknessScaleMax）：CLI 手打越界多半是笔误，
    //   当场早崩并报出合法范围（不 clamp —— 到时渲染侧的 thickness_scales 契约也会崩，
    //   但那条消息说不出“是 --thick-leg 这个参数打错了”）。
    CHECK(p.thick_leg >= jpov_fbx_viewer::kThicknessScaleMin &&
          p.thick_leg <= jpov_fbx_viewer::kThicknessScaleMax)
        << "--thick-leg 取值应在 [" << jpov_fbx_viewer::kThicknessScaleMin << ", "
        << jpov_fbx_viewer::kThicknessScaleMax << "]，got " << p.thick_leg;
    CHECK(p.thick_arm >= jpov_fbx_viewer::kThicknessScaleMin &&
          p.thick_arm <= jpov_fbx_viewer::kThicknessScaleMax)
        << "--thick-arm 取值应在 [" << jpov_fbx_viewer::kThicknessScaleMin << ", "
        << jpov_fbx_viewer::kThicknessScaleMax << "]，got " << p.thick_arm;
    app.thickness_leg_ = p.thick_leg;
    app.thickness_arm_ = p.thick_arm;

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
        if (p.has_view) {
            CHECK_GE(p.view_mode, 0);
            CHECK_LT(p.view_mode, jpov_fbx_viewer::kViewModeItemCount)
                << "--view 取值应为 0.." << (jpov_fbx_viewer::kViewModeItemCount - 1);
            CHECK(p.view_mode == static_cast<int>(jpov_fbx_viewer::ViewMode::kFbxOnly) ||
                  app.has_glb_)
                << "--view 选了含蓝骨的模式但未给 glb 路径（第二位置参数）";
            app.view_mode_ = static_cast<jpov_fbx_viewer::ViewMode>(p.view_mode);
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

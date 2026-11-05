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
//                                / 8 并列(红骨人 + **程序化蓝人模特**，不依赖 glb)
//                                / 9 只看蓝人模特 / 10 只看蓝带皮(真蒙皮，动捕验收用)
//   --time 秒 | --frame 帧号 | --rest
//       可选：--time <秒>   指定动画时刻（默认 0）
//             --frame <n>  指定源帧号（优先级低于 --time；按 fbx 的 fps 换算成时刻）
//             --rest       固定 pose = identity（rest/T-pose）出图
//             --view <n>   指定「显示/驱动」出图（0..9，见 fbx_viewer_app.h 的 ViewMode）
//                          ——与面板 combo 同一套语义。
//             --twist-deg <度>  扭腰（通道 0，±90°）；--tilt-deg <度> 仰头（通道 1，±30°）
//                          —— 与面板滑条同一份语义，仅「蓝带皮」模式生效。
//   → 第二个位置参数（可选）是 **glb 路径**：给了它就多一个「目标骨架（蓝）」——蓝骨 = 从该
//     glb 读出的 rest 骨架，驱动方式由 `--view`（或面板 combo）选：
//     **BodyRetarget**（正式重定向）或**数值直搬**（无重定向对照，用来肉眼验证
//     “不加重定向直接搬 lcl rotation 会不对”）。见 fbx_viewer_app.h 头注 / 设计文档。
//   → `--cloth_path <目录|文件>`（可选）：给已**自动蒙皮**的衣服 glb（目录则加载其中所有
//     *.glb/*.gltf），挂到蓝侧同一骨架 —— 带皮模式下与蓝人**逐帧同步**变形。需配合 glb。
//
// 交互操作：右键 drag 转视角、滚轮 zoom（与模型查看器同款）；**顶部**面板两行 ——
//   控件行（显示/驱动 combo / 暂停按钮 / rest 复选框）+ 状态行。
//   （面板贴顶而非贴底：下拉展开列表向下画，贴底时选项会伸出屏幕外。）
//
// 架构：本主程序只做三件事 —— 解析 CLI、装配 FbxViewerApp、按模式分发
// （交互 Run / headless RunOnce）。渲染与播放逻辑全在 fbx_viewer_app.h，交互与出图
// 共用同一条 OneIteration（zero 分叉）。设计说明见 docs/jpov_fbx_viewer_design.md。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>  // atof / atoi
#include <filesystem>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/viewer_output.h"
#include "tools/jpov/demo/fbx_viewer/fbx_viewer_app.h"
#include "tools/jpov/demo/viewer_render_preference.h"

namespace {

// 命令行解析结果。
struct CliParsed {
    std::string fbx_path;      // 被观察的 FBX（第一个非 "--" 参数）
    std::string glb_path;      // 可选的目标骨架 glb（第二个非 "--" 参数；空 = 不启用对照组）
    std::string cloth_path;    // --cloth_path：已蒙皮衣服 glb（目录/文件；空 = 不穿衣服）
    std::string shot_path;     // 非空 = headless 单帧出图到该路径
    std::string shot_dir;      // 非空 = headless 逐帧出图到该目录（frame_%04d.png）
    std::string video_path;    // 非空 = headless 逐帧渲染后合成 mp4
    int frame_count = 0;       // --frames：视频/序列帧数（0 = 用 clip 帧数）
    int fps = 0;               // --fps：输出帧率（0 = 用 clip fps）
    bool has_time = false;     // --time 是否给出
    double time_seconds = 0.0; // --time 的值（动画时刻，秒）
    bool has_frame = false;    // --frame 是否给出
    int frame_index = 0;       // --frame 的值（源帧号，按 fps 换算时刻）
    bool has_view = false;         // --view 是否给出
    // --view 的值（ViewMode 下标：0 只看红 / 1 只看蓝 / 2 并列 / 3 蓝直搬 /
    //   4 并列直搬 / 5 并列带皮网格 / 6 并列 3 蓝肉人·instanced / 7 并列带皮·粗细微调 /
    //   8 并列蓝人模特 / 9 只看蓝人模特 / 10 只看蓝带皮(真蒙皮)）。
    int view_mode = 0;
    bool rest = false;         // --rest：固定 pose = identity
    // --azimuth-deg / --elev-deg：headless 出图的固定机位（度）。
    //   为什么不加就只能看默认 45° 方位角：AI 看图验收时「正面/侧面」差 45° 会把
    //   T-pose 的胳膊看成「一高一低」，误判成骨架拧了（实测踩过）。
    bool has_azimuth = false;
    double azimuth_deg = 0.0;
    bool has_elev = false;
    double elev_deg = 0.0;
    bool has_azimuth2 = false;
    double azimuth2_deg = 90.0;  // 第二机位（默认侧面 90°）
    // --thick-leg / --thick-arm：部位粗细（组 0 = 腿 / 组 1 = 手臂）系数，
    //   **交互与 headless 出图都生效**（交互时也可继续用面板滑条改）。
    //   与 ViewMode::kSkinnedThickness 的面板滑条是同一份状态。
    float thick_leg = 1.0f;
    float thick_arm = 1.0f;
    // --twist-deg / --tilt-deg：部位额外旋转（通道 0=腰 绕模型上轴，±90°；通道 1=头
    //   绕模型左右轴，±30°）。与 ViewShowsPartialRotationSliders 的面板滑条是同一份状态。
    float twist_deg = 0.0f;
    float tilt_deg  = 0.0f;
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
        } else if (arg == "--azimuth-deg") {
            if (i + 1 < argc) {
                p.azimuth_deg = std::atof(argv[++i]);
                p.has_azimuth = true;
            } else {
                LOG(WARNING) << "--azimuth-deg 缺少数值，忽略";
            }
        } else if (arg == "--elev-deg") {
            if (i + 1 < argc) {
                p.elev_deg = std::atof(argv[++i]);
                p.has_elev = true;
            } else {
                LOG(WARNING) << "--elev-deg 缺少数值，忽略";
            }
        } else if (arg == "--azimuth2-deg") {
            if (i + 1 < argc) {
                p.azimuth2_deg = std::atof(argv[++i]);
                p.has_azimuth2 = true;
            } else {
                LOG(WARNING) << "--azimuth2-deg 缺少数值，忽略";
            }
        } else if (arg == "--video") {
            if (i + 1 < argc) {
                p.video_path = argv[++i];
            } else {
                LOG(WARNING) << "--video 缺少输出路径，忽略";
            }
        } else if (arg == "--shot-dir") {
            if (i + 1 < argc) {
                p.shot_dir = argv[++i];
            } else {
                LOG(WARNING) << "--shot-dir 缺少目录，忽略";
            }
        } else if (arg == "--frames") {
            if (i + 1 < argc) {
                p.frame_count = std::atoi(argv[++i]);
            } else {
                LOG(WARNING) << "--frames 缺少数值，忽略";
            }
        } else if (arg == "--fps") {
            if (i + 1 < argc) {
                p.fps = std::atoi(argv[++i]);
            } else {
                LOG(WARNING) << "--fps 缺少数值，忽略";
            }
        } else if (arg == "--rest") {
            p.rest = true;
        } else if (arg == "--cloth_path") {
            if (i + 1 < argc) {
                p.cloth_path = argv[++i];
            } else {
                LOG(WARNING) << "--cloth_path 缺少路径参数，忽略";
            }
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
        } else if (arg == "--twist-deg") {
            if (i + 1 < argc) {
                p.twist_deg = static_cast<float>(std::atof(argv[++i]));
            } else {
                LOG(WARNING) << "--twist-deg 缺少数值，忽略";
            }
        } else if (arg == "--tilt-deg") {
            if (i + 1 < argc) {
                p.tilt_deg = static_cast<float>(std::atof(argv[++i]));
            } else {
                LOG(WARNING) << "--tilt-deg 缺少数值，忽略";
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
           "[--time 秒|--frame 帧号] [--rest] [--view 0..10] "
           "[--thick-leg 0.3~2.0] [--thick-arm 0.3~2.0] "
           "[--twist-deg -90~90] [--tilt-deg -30~30]";
    const bool capture = !p.shot_path.empty() || !p.shot_dir.empty() ||
                         !p.video_path.empty();
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
    if (!capture) {
        jpov_viewer::PreferGpuForInteractiveViewer(cfg);
    }
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
    if (!p.cloth_path.empty()) {
        // 已蒙皮衣服：挂到蓝侧同一骨架（需先有蓝骨；骨序须一致，见 LoadCloths）。
        CHECK(!p.glb_path.empty())
            << "--cloth_path 需配合第二个位置参数（目标骨架 glb）一起用";
        CHECK(app.LoadCloths(p.cloth_path) > 0)
            << "衣服挂载失败（0 个 primitive 挂上）：" << p.cloth_path;
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

    // 部位额外旋转角度（与面板滑条同一份状态）：交互与出图都生效。
    //   值域与面板滑条一致（±kPartialTwistMaxDeg / ±kPartialTiltMaxDeg）：CLI 手打越界多半是
    //   笔误，当场早崩并报出合法范围（不 clamp）。
    CHECK(p.twist_deg >= -jpov_fbx_viewer::kPartialTwistMaxDeg &&
          p.twist_deg <= jpov_fbx_viewer::kPartialTwistMaxDeg)
        << "--twist-deg 取值应在 [" << -jpov_fbx_viewer::kPartialTwistMaxDeg << ", "
        << jpov_fbx_viewer::kPartialTwistMaxDeg << "], got " << p.twist_deg;
    CHECK(p.tilt_deg >= -jpov_fbx_viewer::kPartialTiltMaxDeg &&
          p.tilt_deg <= jpov_fbx_viewer::kPartialTiltMaxDeg)
        << "--tilt-deg 取值应在 [" << -jpov_fbx_viewer::kPartialTiltMaxDeg << ", "
        << jpov_fbx_viewer::kPartialTiltMaxDeg << "], got " << p.tilt_deg;
    app.partial_twist_waist_deg_ = p.twist_deg;
    app.partial_tilt_head_deg_   = p.tilt_deg;

    if (capture) {
        CHECK(p.shot_path.empty() || (p.shot_dir.empty() && p.video_path.empty()))
            << "--shot / --shot-dir / --video 只能给一个";
        CHECK(p.video_path.empty() || p.shot_dir.empty())
            << "--video 与 --shot-dir 只能给一个";
        CHECK((p.video_path.empty() && p.shot_dir.empty()) ||
              (!p.has_time && !p.has_frame))
            << "--time / --frame 只用于单帧 --shot；序列/视频请用 --frames";
        app.rest_pose_mode_ = p.rest;
        if (p.has_view) {
            CHECK_GE(p.view_mode, 0);
            CHECK_LT(p.view_mode, jpov_fbx_viewer::kViewModeItemCount)
                << "--view 取值应为 0.." << (jpov_fbx_viewer::kViewModeItemCount - 1);
            CHECK(p.view_mode == static_cast<int>(jpov_fbx_viewer::ViewMode::kFbxOnly) ||
                  p.view_mode == static_cast<int>(jpov_fbx_viewer::ViewMode::kBothMannequin) ||
                  p.view_mode == static_cast<int>(jpov_fbx_viewer::ViewMode::kMannequinOnly) ||
                  app.has_glb_)
                << "--view 选了含蓝骨的模式但未给 glb 路径（第二位置参数；"
                   "只有 --view 8 蓝人模特不需要 glb）";
            app.view_mode_ = static_cast<jpov_fbx_viewer::ViewMode>(p.view_mode);
            // --view 在装配之后才定：机位按最终模式重算（“只看真皮”不该按并列布局取景）。
            app.RefitViewForMode();
        }
        // 机位角度必须在 RefitViewForMode **之后**设：重算机位 = 回到 DefaultView（theta/phi 归位）。
        if (p.has_azimuth) {
            app.view_.theta = p.azimuth_deg * 3.14159265358979323846 / 180.0;
        }
        if (p.has_elev) {
            app.view_.phi = p.elev_deg * 3.14159265358979323846 / 180.0;
        }
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(jpov_fbx_viewer::kViewerWidth);
        winfo.height = static_cast<float>(jpov_fbx_viewer::kViewerHeight);
        const jpov::InputSnapshot input{};

        if (!p.video_path.empty() || !p.shot_dir.empty()) {
            const double clip_fps = std::max(1.0, app.clip_.frames_per_second);
            const int out_fps = p.fps > 0 ? p.fps : static_cast<int>(std::lround(clip_fps));
            const int out_frames = p.frame_count > 0
                                       ? p.frame_count
                                       : static_cast<int>(app.clip_.frames.size());
            CHECK_GT(out_fps, 0);
            CHECK_GT(out_frames, 0);
            const double kDeg2Rad = 3.14159265358979323846 / 180.0;
            const double az1 = p.has_azimuth ? p.azimuth_deg : 0.0;
            const double az2 = p.has_azimuth2 ? p.azimuth2_deg : az1;

            auto render_sequence = [&](const std::string& dir, double azimuth_deg) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                CHECK(!ec) << "创建帧目录失败: " << dir << " (" << ec.message() << ")";
                app.view_.theta = azimuth_deg * kDeg2Rad;
                for (int i = 0; i < out_frames; ++i) {
                    app.anim_time_seconds_ = static_cast<double>(i) / out_fps;
                    char name[32];
                    std::snprintf(name, sizeof(name), "frame_%04d.png", i);
                    const std::string frame_path =
                        (std::filesystem::path(dir) / name).string();
                    app.RunOnce(static_cast<int64_t>(i), input, winfo,
                                frame_path.c_str());
                }
            };

            if (!p.video_path.empty()) {
                const std::filesystem::path video_path(p.video_path);
                if (video_path.has_parent_path()) {
                    std::error_code ec;
                    std::filesystem::create_directories(video_path.parent_path(), ec);
                    CHECK(!ec) << "创建视频目录失败: "
                               << video_path.parent_path().string()
                               << " (" << ec.message() << ")";
                }
                const std::string stem = video_path.stem().string();
                const std::filesystem::path base_dir =
                    video_path.has_parent_path() ? video_path.parent_path()
                                                 : std::filesystem::path(".");
                const std::string dir_a = (base_dir / (stem + "_frames_a")).string();
                if (p.has_azimuth2) {
                    const std::string dir_b = (base_dir / (stem + "_frames_b")).string();
                    render_sequence(dir_a, az1);
                    render_sequence(dir_b, az2);
                    const std::string mp4_a = (base_dir / (stem + "_a.mp4")).string();
                    const std::string mp4_b = (base_dir / (stem + "_b.mp4")).string();
                    CHECK(jpov_viewer::RunFfmpeg(dir_a + "/frame_%04d.png", mp4_a, out_fps))
                        << "角度 A 合成失败";
                    CHECK(jpov_viewer::RunFfmpeg(dir_b + "/frame_%04d.png", mp4_b, out_fps))
                        << "角度 B 合成失败";
                    CHECK(jpov_viewer::RunFfmpegHstack(mp4_a, mp4_b, p.video_path))
                        << "两角度并排合成失败";
                    jpov_viewer::RemoveDirRecursive(dir_a);
                    jpov_viewer::RemoveDirRecursive(dir_b);
                    std::error_code ec;
                    std::filesystem::remove(mp4_a, ec);
                    std::filesystem::remove(mp4_b, ec);
                } else {
                    render_sequence(dir_a, az1);
                    CHECK(jpov_viewer::RunFfmpeg(dir_a + "/frame_%04d.png",
                                                 p.video_path, out_fps))
                        << "视频合成失败";
                    jpov_viewer::RemoveDirRecursive(dir_a);
                }
                LOG(INFO) << "已出视频: " << p.video_path
                          << " (" << out_frames << " frames @ " << out_fps << "fps)";
            } else {
                render_sequence(p.shot_dir, az1);
                LOG(INFO) << "已出帧序列: " << p.shot_dir;
            }
        } else {
            // headless 单帧：先把时间设好，再 RunOnce（OneIteration 画的是"推进前"
            // 的时刻，故设定值即出图时刻）。
            if (p.has_time) {
                app.anim_time_seconds_ = p.time_seconds;
            } else if (p.has_frame) {
                app.anim_time_seconds_ = static_cast<double>(p.frame_index) /
                                         app.clip_.frames_per_second;
            }
            app.RunOnce(input, winfo, p.shot_path.c_str());
            LOG(INFO) << "已出图: " << p.shot_path;
        }
    } else {
        app.Run();  // 交互事件循环（阻塞）
    }

    app.Finalize();
    return 0;
}

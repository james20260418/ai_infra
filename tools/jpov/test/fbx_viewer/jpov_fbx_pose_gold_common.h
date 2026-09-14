// JPOV FBX 观察器 — 固定帧渲染 gold 的公共场景
//
// 目的：把"FBX 动作 → 火柴人"这条渲染链路钉住一张仓库 gold 图（某个固定时刻的帧），
// 作为"从 FBX 的 Lcl Rotation 构造出的 pose 能被正确渲成人形动作"的视觉回归门禁。
//
// 关键：本 gold **渲染的就是观察器本体**（demo/fbx_viewer 的 FbxViewerApp，headless、
// 不画面板、时间固定在某一帧），不是另写一份"长得像"的场景 —— 于是图中所示即用户
// 跑起来看到的，generator 与 test 也不可能分叉（同一个 SetupApp）。
//
// 本头文件被 generator（写仓库 gold image）与 test（渲染 + 门禁）共用。

#ifndef JPOV_TEST_FBX_VIEWER_JPOV_FBX_POSE_GOLD_COMMON_H_
#define JPOV_TEST_FBX_VIEWER_JPOV_FBX_POSE_GOLD_COMMON_H_

#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/fbx_viewer/fbx_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/test_utils.h"

namespace jpov_fbx_pose_gold {

// gold image 仓库相对路径（generator 写入、test 读取比对）。
inline std::string GetGoldRelPath() {
    return "/fbx_viewer/fbx_dance_frame_1280x720.png";
}

// 第二张 gold：传了 glb 的「对照组」（两者并列，红左/蓝右）——蓝 = glb rest 骨架
// 被同一份 fbx pose **数值直搬**（无重定向）驱动。
inline std::string GetGlbNaiveGoldRelPath() {
    return "/fbx_viewer/fbx_pose_glb_naive_1280x720.png";
}

// 出图尺寸（winfo，单位像素）。与其它 gold 同规格：3D FBO 恒为 1280×720，
// 落盘用 640×360（省仓库体积）。
inline constexpr float kShotWidth  = 640.0f;
inline constexpr float kShotHeight = 360.0f;

// gold 采样时刻（秒）：2.0s = 第 60 帧（源 30fps 的帧网格上）—— 舞中段、姿态舒展。
inline constexpr double kGoldTimeSeconds = 2.0;

// 第二个时刻（秒）：5.0s = 第 150 帧。用于"两帧必须不一样"的动态门禁
// （若位姿→几何的更新链断了，两帧会渲成同一张图）。
inline constexpr double kOtherTimeSeconds = 5.0;

// 被观察的 FBX（经 Bazel data 提供；GetTestDataDir 兼容 bazel test/run 两态）。
inline std::string FbxPath() {
    return jpov::GetTestDataDir() + "/animations/hip_hop_dance.fbx";
}

// 可选目标骨架 glb（Tripo 23 骨；同上经 Bazel data 提供）。
inline std::string GlbPath() {
    return jpov::GetTestDataDir() + "/object3d/mixamo_male/mixamo_male.glb";
}

// 出图用配置：headless（不弹窗）、1280×720、60fps、只注册出图需要的拉丁字体
// （headless 不画面板，用不到 CJK）。
// 说明：JPOV 不提供隐式默认字体，未注册字体则 Init 失败 —— 故这里显式给一条。
inline JPOV::Config MakeConfig(const char* title) {
    JPOV::Config cfg;
    cfg.title  = title;
    cfg.width  = jpov_fbx_viewer::kViewerWidth;
    cfg.height = jpov_fbx_viewer::kViewerHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_fbx_viewer::kViewerFps);
    cfg.headless   = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    return cfg;
}

// 造并装配一个出图用 App（generator 与 test **共用本入口**，防分叉）。
//   glb_path 非空 → 同时装目标骨架（蓝）并选定 mesh_source；
//   注意：LoadGltf 会按“两者并列”重算初始机位，故“含 glb”与“不含 glb”是两种不同机位。
// Pre-condition: FBX 路径可读且带动画；glb_path 非空时可读且有 skin。
inline std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> MakeApp(
    const char* title, double time_seconds, bool rest_pose,
    const std::string& glb_path, jpov_fbx_viewer::MeshSource mesh_source) {
    JPOV::Config cfg = MakeConfig(title);
    std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app(
        new jpov_fbx_viewer::FbxViewerApp(cfg));
    app->SetShowPanel(false);  // 出图是纯 3D 截图，不画面板、不消费输入
    app->Init();
    CHECK(app->LoadFbx(FbxPath())) << "装配 FBX 失败: " << FbxPath();
    if (!glb_path.empty()) {
        CHECK(app->LoadGlbSkeleton(glb_path))
            << "装配目标骨架 glb 失败: " << glb_path;
        app->mesh_source_ = mesh_source;  // 覆盖 LoadGlbSkeleton 的默认（kBoth）
    }
    app->anim_time_seconds_ = time_seconds;
    app->rest_pose_mode_    = rest_pose;
    return app;  // 析构时 JPOV::~JPOV 会 Finalize()
}

// 渲一帧到 out_png（走观察器自己的 OneIteration，零分叉）。
inline void RenderFrame(jpov_fbx_viewer::FbxViewerApp* app /*inout*/,
                        const char* out_png) {
    CHECK(app != nullptr) << "RenderFrame: app 不能为空";
    jpov::WindowInfo winfo;
    winfo.width  = kShotWidth;
    winfo.height = kShotHeight;
    const jpov::InputSnapshot input{};
    app->RunOnce(input, winfo, out_png);
}

}  // namespace jpov_fbx_pose_gold

#endif  // JPOV_TEST_FBX_VIEWER_JPOV_FBX_POSE_GOLD_COMMON_H_

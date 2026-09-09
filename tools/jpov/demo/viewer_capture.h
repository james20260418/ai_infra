// JPOV 模型查看器 — headless 拍摄模式 driver（four_views / round_video）
//
// 架构治理动机（重构，见 docs/jpov_model_viewer_arch.md）：历史形态里
// RunFourViews 与 RunRoundVideo 独立实现，各自重复“dir/base 推导 + mkdir 输出目录 +
// winfo 尺寸 + 设 view_ 后 RunOnce 出图”~30 行；下一种拍摄模式（如 round_gif）会
// 再抄第四次（SOUL 忌讳 demo 分叉）。本次抽出本 driver 统一“拍摄 = 一系列视角 ×
// 逐个渲染成 {PNG 或 mp4}”。
//
// 分工：
//   CaptureViewerScene —— 在给定 ViewerApp 上做一次像素输出。两种形态：
//     four_views  : 一组固定视角(View) → 每视角一张 PNG
//     round_video : θ 从 0→π 扫 num_frames 帧 → PNG 临时缓冲 → ffmpeg 合成 mp4
//   两者原先是并列函数；重构为一张 `CaptureSpec`（见 RunCapture）表达的单一驱动：
//   统一解析输出目录/文件名前缀 → 建目录 → 设 view_ 逐帧 RunOnce → （round 才有
//   的镜像：合 mp4 + 清临时帧目录）。渲染细节一律走 ViewerApp::OneIteration（zero
//   分叉），本文件不重复任何渲染/相机逻辑。
//
// 纯文件系统/ffmpeg 工具的职责在 viewer_output.h，不在此重复。

#ifndef JPOV_DEMO_VIEWER_CAPTURE_H_
#define JPOV_DEMO_VIEWER_CAPTURE_H_

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/demo/viewer_output.h"

namespace jpov_viewer {

// headless 拍摄的产物形态。
enum class CaptureKind {
    kFourViews,   // 4 个固定角度 → 各一张 PNG
    kRoundVideo,  // θ 扫 180° → 合成一个 mp4
};

// 一次拍摄的完整描述（含 CLI 传入参数拍平后的值，供 main 直接构造）。
struct CaptureSpec {
    CaptureKind kind = CaptureKind::kFourViews;

    // 两形态共用：
    std::string gltf_path;         // 被拍模型（决定输出 basename：<base>_*）
    std::string output_dir;        // --output_dir（可为空 = 退回 glTF 同级目录）
    double      phi_deg = 45.0;    // round 固定仰角 / four_views 的 perspective 角里
                                   // 只有 round 用（four_views 各视角写在 kFourView 表）。

    // 仅 round_video 用：
    int   num_frames = 60;         // θ 0→π 扫的总帧数
    int   fps        = 10;         // 视频帧率（60 帧 @10fps = 6s）
};

// four_views 的 4 个固定拍照角度 (phi, theta)，单位度（需求定稿）。
struct FourViewAngle {
    const char* name;   // 文件名后缀（front/up/left/perspective）
    double phi_deg;
    double theta_deg;
};
inline constexpr FourViewAngle kFourViewAngles[] = {
    {"front",       0.0,   0.0},   // (φ,θ)=(0,0)
    {"up",         90.0,   0.0},   // (φ,θ)=(90,0)
    {"left",        0.0,  90.0},   // (φ,θ)=(0,90)
    {"perspective",45.0,  45.0},   // (φ,θ)=(45,45)
};

// 度数 → 弧度（ViewConfig 以弧度存视角）。
inline constexpr double kViewerDegToRad = 3.14159265358979323846 / 180.0;

// 把当前 app 相机设成给定 (phi,theta)（弧度），R 保持 app.view_.R 不变
// （main 已按模型包围盒 FitRadius 自适应好；拍摄沿用同一取景距离才有连贯画面）。
inline void SetCaptureView(ViewerApp* app, double phi, double theta) {
    app->view_.phi   = phi;
    app->view_.theta = theta;
    // R 防御 clamp（理论在此已合法，显式兜底保证 FitRadius/DefaultView 之外的赋值
    // 也不会把相机推出 kRMin..kRMax 合法带，防未来误改）。
    app->view_.R = std::clamp(app->view_.R, ViewConfig::kRMin,
                              ViewConfig::kRMax);
}

// 在给定 app 上执行一次拍摄（spec 描述产物形态与参数）。渲染细节全走
// ViewerApp::OneIteration；本函数只负责“编排视角序列 + 落盘/合成”。
// spec.kind == kFourViews → 打印每张 PNG 完整路径到 stdout；
//           == kRoundVideo → 循环里 print 进度，结束打印 mp4 完整路径到 stdout。
// 返回 false 表示拍摄失败（已 LOG ERROR；调用方决定是否 LOG(FATAL)）。
bool RunCapture(ViewerApp* app, const CaptureSpec& spec) {
    CHECK_NOTNULL(app);

    // 输出目录 / basename（统一推导，杜绝两形态各写一遍）。
    const OutputTarget out = ResolveOutputTarget(spec.gltf_path, spec.output_dir);
    if (!EnsureOutputDir(out.dir)) {
        LOG(ERROR) << "无法创建输出目录: " << out.dir;
        return false;
    }
    const std::string& dir  = out.dir;
    const std::string& base = out.base;

    // 无论哪种形态，每次 RunOnce 都用同一份空输入（headless 无交互）。
    // ViewerApp 会在 show_panel_=false 下忽略它（相机由 SetCaptureView 显式控制）。
    const jpov::InputSnapshot input{};
    jpov::WindowInfo winfo;
    winfo.width  = kViewerWidth;
    winfo.height = kViewerHeight;

    if (spec.kind == CaptureKind::kFourViews) {
        for (const FourViewAngle& fv : kFourViewAngles) {
            const std::string out_png = dir + "/" + base + "_" + fv.name + ".png";
            SetCaptureView(app, fv.phi_deg * kViewerDegToRad,
                           fv.theta_deg * kViewerDegToRad);
            app->RunOnce(input, winfo, out_png.c_str());
            std::printf("%s\n", out_png.c_str());  // 便于脚本/用户直取路径
            LOG(INFO) << "--four_views 已渲染: " << out_png;
        }
        return true;
    }

    // ---- round_video：θ 从 0→π 均匀扫 num_frames 帧，合成 mp4 ----
    CHECK(spec.num_frames > 0) << "--frames 必须 > 0: " << spec.num_frames;
    CHECK(spec.fps > 0) << "--fps 必须 > 0: " << spec.fps;

    // 临时 PNG 缓冲目录（输出目录下），合成完递归清理。
    std::string frames_dir;
    if (!EnsureTempFramesDir(dir, base, &frames_dir)) return false;

    // 相机仰角（φ）固定 = phi_deg，clamp 到 ViewConfig 合法范围 [-90°,90°]。
    constexpr double kPi      = 3.14159265358979323846;
    constexpr double kPiHalf  = kPi / 2.0;
    const double phi_clamped = std::clamp(spec.phi_deg * kViewerDegToRad,
                                          -kPiHalf, kPiHalf);
    // 从正面 θ=0 扫到背面 θ=π。R 保持 main 的 FitRadius 自适应值（框住全模型）。
    SetCaptureView(app, phi_clamped, /*theta*/ 0.0);

    LOG(INFO) << "--round_video: frames=" << spec.num_frames
              << ", theta 0°→180°, phi=" << spec.phi_deg
              << "°, tmp=" << frames_dir;
    for (int i = 0; i < spec.num_frames; ++i) {
        // θ = i/(frames-1)·π：末帧到 π（180° 背面）；仅 1 帧时 θ=0。
        const double t = (spec.num_frames == 1)
                             ? 0.0
                             : static_cast<double>(i) / (spec.num_frames - 1);
        SetCaptureView(app, phi_clamped, t * kPi);

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.png",
                      static_cast<int>(i));
        const std::string out_png = frames_dir + "/" + name;
        app->RunOnce(input, winfo, out_png.c_str());
        if ((i % 10) == 0 || i == spec.num_frames - 1) {
            LOG(INFO) << "  [" << (i + 1) << "/" << spec.num_frames
                      << "] theta=" << (t * 180.0) << "°";   // t∈[0,1]→0°~180°
        }
    }

    // 全部渲染成功后才合成 mp4；无论 ffmpeg 成败，临时帧目录都清（不留 PNG 垃圾；
    // 失败靠 RunFfmpeg 的完整 LOG 排查）。缺 ffmpeg 时这里同样能把临时目录清干净。
    const std::string out_mp4  = dir + "/" + base + "_round.mp4";
    const std::string pattern  = frames_dir + "/frame_%04d.png";
    const bool ok = RunFfmpeg(pattern, out_mp4, spec.fps);
    RemoveDirRecursive(frames_dir);
    if (!ok) {
        LOG(ERROR) << "ffmpeg 合成失败，未产生 " << out_mp4;
        return false;
    }
    std::printf("%s\n", out_mp4.c_str());
    LOG(INFO) << "--round_video 已合成: " << out_mp4;
    return true;
}

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_VIEWER_CAPTURE_H_

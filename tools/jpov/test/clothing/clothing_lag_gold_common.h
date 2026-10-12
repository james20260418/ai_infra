// 布料 LAG gold 公共场景 —— generator(写 gold) 与 test(比对) 共用。
//
// 用**裙子资产**（lace_skirt_skinned_relaxed_sample.glb，自带 _JPOV_RELAX 松弛度）在**固定设定**
//   下出图：mixamo_male + 裙子；随机动作相位/幅度/种子固定；整体平动（世界 XY）经 warmup ramp
//   到固定目标（让平动 LAG 的 tracker 起滞后）；LAG 开/关、一/二阶可切。
// 目的（Danis 2026-10-12）：给「**裙摆飘起**这件事**加单测保护**」—— LAG 让裙子（松弛部分）
//   滞后于整体平动 ⇒ 裙摆相对人体「飘」出 `relax·pos_lag` 的位移。
#ifndef JPOV_TEST_CLOTHING_CLOTHING_LAG_GOLD_COMMON_H_
#define JPOV_TEST_CLOTHING_CLOTHING_LAG_GOLD_COMMON_H_

#include <chrono>
#include <string>
#include <thread>

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_tool_app.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/test_utils.h"

namespace jpov_cloth_lag_gold {

constexpr int kW = 640;       // 出图宽（小图，快）
constexpr int kH = 360;       // 出图高
constexpr int kWarmup = 40;   // warmup 帧数：把整体平动 ramp 到目标（让 LAG tracker 起滞后）
constexpr float kTransX = 1.0f;  // 整体平动目标 X（m）
constexpr float kTransY = 0.3f;  // 整体平动目标 Y（m）

inline std::string GoldRelPath() { return "/clothing/cloth_lag_640x360.png"; }
inline std::string BodyPath() {
    return jpov::GetModelsDir() + "/characters/mixamo_male.glb";
}
inline std::string ClothPath() {
    return jpov::GetModelsDir() +
           "/clothing/lace_skirt/lace_skirt_skinned_relaxed_sample.glb";
}

// 渲一帧（LAG on/off、一/二阶）到 out_png。同参数 ⇒ 同帧（generator 与 test 共用）。
inline bool RenderClothLag(bool lag_on, bool second_order, const std::string& out_png) {
    JPOV::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.target_fps = 60;
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov::clothing::ClothingToolApp app(cfg);
    app.SetShowPanel(false);  // 纯 3D（gold 不含面板）
    app.Init();
    app.InstallTextMeasure();
    app.body_path_ = BodyPath();
    app.cloth_path_ = ClothPath();
    app.init_.Start(app.body_path_, app.cloth_path_);
    app.ground_mat_ = jpov::clothing::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov::clothing::MakeGroundQuad());
    app.view_ = jpov::clothing::DefaultView();
    app.view_.phi = 20.0 * 3.14159265358979323846 / 180.0;
    // 随机动作（固定 → 帧确定）。
    app.motion_test_enabled_ = true;
    app.motion_phase_ = 0.2f;
    app.motion_amplitude_deg_ = 45.0f;
    app.motion_seed_ = 0;
    // 平动 LAG。
    app.lag_enabled_ui_ = lag_on;
    app.lag_second_order_ui_ = second_order;
    app.lag_rise_ms_ui_ = 200.0f;
    app.lag_overshoot_ui_ = 0.2f;
    // 等场景就绪（后台建 matcher + GPU 上传）。
    while (!app.TickInit()) {
        if (app.init_.state() == jpov::clothing::InitState::kFailed) {
            LOG(ERROR) << "初始化失败: " << app.init_.error_message();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 拉近相机（场景就绪后 FitViewToScene 已定 R）：让「裙摆飘移 max_lag(0.1m)」在屏幕上占更多
    //   像素 ⇒ 回归门禁更灵敏（太小会被 ROI 平均掉）。
    app.view_.R *= 0.55;
    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kW);
    winfo.height = static_cast<float>(kH);
    // warmup：整体平动从 0 线性 ramp 到目标 ⇒ tracker 追踪运动中的目标，末帧 pos_lag≠0。
    for (int i = 1; i <= kWarmup; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(kWarmup);
        app.body_trans_ui_[0] = kTransX * f;
        app.body_trans_ui_[1] = kTransY * f;
        app.RunOnce(jpov::InputSnapshot(), winfo, "/tmp/jpov_cloth_lag_warmup.png");
    }
    app.RunOnce(jpov::InputSnapshot(), winfo, out_png.c_str());
    app.Finalize();
    LOG(INFO) << "cloth LAG gold 渲图: " << out_png << "（lag=" << lag_on
              << ", second=" << second_order << "）";
    return true;
}

}  // namespace jpov_cloth_lag_gold

#endif  // JPOV_TEST_CLOTHING_CLOTHING_LAG_GOLD_COMMON_H_

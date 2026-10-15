// JPOV 植被风查看器 — 渲染核心 App。
//
// 用途：肉眼验收 **VS 植被风**（无状态程序化顶点风）。模特 = 小柳树
//   （tools/vegetation/samples/willow_small_tex.glb，加载时派生 bend/phase）。
//
// 一帧 = 环境态 `cmds->wind`（time/amplitude/frequency/turbulence）+ 一排柳树实例
//   （per-instance wind.xyz + phase）。绘制走 **instanced** 路径（per-instance 风场）。
//
// 设计：docs/jpov_vegetation_wind_design.md。

#ifndef JPOV_DEMO_WIND_VIEWER_APP_H_
#define JPOV_DEMO_WIND_VIEWER_APP_H_

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/common/utils.h"   // jpov::ResolveResourcePath
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/render_command.h"   // InstanceState / WindCommand
#include "tools/jpov/interface/ui.h"

namespace jpov_wind {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 确定性哈希 → [0,1)。仅用于每实例相位/风向扰动（gold 友好）。与 wind_flex.h 同款。
inline float Hash01(int k, int salt) {
    uint32_t h = static_cast<uint32_t>(k) * 2654435761u ^
                 static_cast<uint32_t>(salt) * 40503u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

class WindViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景资源 ──
    jpov::GltfObject willow_;        // 小柳树（derive_wind_flex=true 载入）
    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;

    // ── 视角 ──
    jpov_viewer::ViewConfig view_;

    // ── 风参数（面板可调）──
    float wind_azimuth_deg_ = 35.0f;   // 风向方位角（绕 +Y，度）
    float wind_speed_ = 1.6f;          // 风速
    float amplitude_ = 0.6f;           // 主弯振幅（米/单位）
    float frequency_ = 0.9f;           // 基频（Hz）
    float turbulence_ = 0.5f;          // 细节颤幅度 [0,1]
    float time_scale_ = 1.0f;          // 时间缩放（0 = 暂停）
    float model_scale_ = 1.0f;         // 小柳树整体缩放

    // ── 风场（每实例扰动）──
    int   instance_count_ = 5;         // 一排几株
    float spacing_ = 3.0f;             // 株距（米）
    float jitter_ = 0.6f;              // 每株风向/风速/相位的扰动强度 [0,1]

    // ── 时间（确定性：由 frame_count 推；或 headless 固定某秒）──
    bool  use_fixed_time_ = false;
    float fixed_time_ = 0.0f;
    bool  wind_enabled_ = true;

    // 装配：地面 + 载入小柳树。Init() 后调用一次。
    void InstallScene() {
        ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
        ground_mat_ = jpov_skylight::GroundMaterial();
        const std::string path = jpov::ResolveResourcePath("willow_small_tex.glb");
        willow_ = LoadGltf(path, /*derive_wind_flex=*/true);
        CHECK(!willow_.empty()) << "WindViewerApp: 小柳树加载失败: " << path;
    }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&WindViewerApp::AppTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }
    // headless：固定时间（秒），用于逐帧拍摄。
    void SetFixedTime(float t) {
        use_fixed_time_ = true;
        fixed_time_ = t;
    }
    // 开关风（headless A/B：关 = 不设 cmds->wind，恒静止）。
    void SetWindEnabled(bool on) { wind_enabled_ = on; }

    // 本帧风的合成矢量（单位方向 × 风速），供 object3d（地面）用；实例路径另取 per-instance。
    jpov::Vec3f SceneWindDir() const {
        const float az = wind_azimuth_deg_ * 3.14159265358979323846f / 180.0f;
        return jpov::Vec3f(std::cos(az), 0.0f, std::sin(az));
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        if (show_panel_) {
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
        }

        cmds->camera.position = view_.Position();
        cmds->camera.target   = {0.0f, 1.6f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 55.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 500.0f;

        const jpov_viewer::NoonLighting nl = jpov_viewer::MakeNoonLighting();
        cmds->sky     = nl.sky;
        cmds->sun     = nl.sun;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 确定性时间：优先固定值（headless），否则由帧号推。
        const float t = use_fixed_time_
                            ? fixed_time_
                            : static_cast<float>(frame_count) / kViewerFps * time_scale_;

        // 环境风（全局标量；方向/风速供 object3d 路径，实例路径取 per-instance）。
        jpov::WindCommand wind;
        wind.time = t;
        wind.amplitude = amplitude_;
        wind.frequency = frequency_;
        wind.turbulence = turbulence_;
        wind.direction = SceneWindDir();
        wind.speed = wind_speed_;
        if (wind_enabled_) {
            cmds->wind = wind;
        }

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  4.0f);

        // 一排柳树实例：每株自己的风矢量 + 相位（风场）。
        if (!willow_.empty()) {
            std::vector<jpov::InstanceState> instances;
            instances.reserve(static_cast<size_t>(instance_count_));
            const float x0 = -0.5f * spacing_ * static_cast<float>(instance_count_ - 1);
            for (int k = 0; k < instance_count_; ++k) {
                jpov::InstanceState st;
                st.transform.center =
                    {x0 + spacing_ * static_cast<float>(k), 0.0f, 0.0f};
                st.transform.up    = {0.0f, 1.0f, 0.0f};
                st.transform.front = {0.0f, 0.0f, 1.0f};
                st.transform.scale = model_scale_;

                // 每株：风向/风速略扰动 + 相位错开 → 风场。
                const float j1 = (Hash01(k, 1) * 2.0f - 1.0f) * jitter_;
                const float j2 = (Hash01(k, 2) * 2.0f - 1.0f) * jitter_;
                const float az = wind_azimuth_deg_ * 3.14159265358979323846f / 180.0f +
                                 j1 * 0.6f;
                const float sp = wind_speed_ * (1.0f + j2 * 0.5f);
                st.wind = {std::cos(az) * sp, 0.0f, std::sin(az) * sp};
                st.wind_phase = Hash01(k, 3) * jitter_ * 2.0f;   // 秒
                instances.push_back(st);
            }
            for (const jpov::GltfPrimitive& prim : willow_.primitives) {
                cmds->DrawInstancedObject(prim.mesh_id, prim.material, instances);
            }
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        WindViewerApp* app = static_cast<WindViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float kWidth  = 0.5f * w;
        const float kRowH   = 26.0f;
        const float kSpace  = 10.0f;
        const float kBottom = 20.0f;
        const float left    = (w - kWidth) * 0.5f;
        const int   kRows   = 11;
        const float top     = h - kBottom -
                              (static_cast<float>(kRows) * kRowH +
                               static_cast<float>(kRows - 1) * kSpace);
        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpace)},
                                {kWidth, kRowH}};
        };

        ui_.SliderFloat("风向方位角 deg", &wind_azimuth_deg_, row(0), 0.0f, 360.0f, 0);
        ui_.SliderFloat("风速", &wind_speed_, row(1), 0.0f, 6.0f, 2);
        ui_.SliderFloat("振幅", &amplitude_, row(2), 0.0f, 3.0f, 2);
        ui_.SliderFloat("频率 Hz", &frequency_, row(3), 0.1f, 4.0f, 2);
        ui_.SliderFloat("湍流（细节颤）", &turbulence_, row(4), 0.0f, 1.5f, 2);
        float ts_f = time_scale_;
        if (ui_.SliderFloat("时间缩放（0=暂停）", &ts_f, row(5), 0.0f, 3.0f, 2)) {
            time_scale_ = ts_f;
        }
        ui_.SliderFloat("株距 m", &spacing_, row(6), 1.0f, 6.0f, 2);
        ui_.SliderFloat("每株扰动", &jitter_, row(7), 0.0f, 1.5f, 2);
        float ic_f = static_cast<float>(instance_count_);
        if (ui_.SliderFloat("株数", &ic_f, row(8), 1.0f, 12.0f, /*dec*/0)) {
            instance_count_ = static_cast<int>(ic_f + 0.5f);
        }
        ui_.SliderFloat("模型缩放", &model_scale_, row(9), 0.3f, 3.0f, 2);
        char info[128];
        std::snprintf(info, sizeof(info), "小柳树 × %d  |  风 VS（%s）",
                      instance_count_, use_fixed_time_ ? "固定时间" : "实时");
        ui_.Text(info, row(10));
    }

    bool show_panel_ = true;
    jpov::Ui ui_;
    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace jpov_wind

#endif  // JPOV_DEMO_WIND_VIEWER_APP_H_

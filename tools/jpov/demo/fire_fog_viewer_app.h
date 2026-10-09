// JPOV 雾火查看器（fire_fog viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 目的：**肉眼验收点状雾（fire-fog MVP）**的最小测试场。场景 = 灰色地面 + 一棵高模橡树
//（oak），点状雾默认摆在橡树处（便于观察雾与实体的深度关系：雾在树前 / 树后被裁剪）。
//
// 与 skylight viewer 同构（同一套 JPOV::OneIteration 渲染体 + 即时模式 UI 面板约定）：
//   - 场景资源（地面 mesh / 橡树 glTF）由主程序装配后传入；
//   - 光照沿用 skylight_scene.h 的 SkyDegrees + MakeSkyLighting（白昼标准天光，单色 ambient）；
//   - 交互面板暴露**点状雾**的可调参数：中心 x/y/z、半径、强度、颜色、衰减剖面；
//     另有一行只读信息显示 JPOV 锁定的内部量（M / K / tile）。
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + 右键拖拽/滚轮缩放）。

#ifndef JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_
#define JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"   // 复用 SkyDegrees / MakeSkyLighting / 地面材质
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"       // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/ui.h"

namespace jpov_fire_fog {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 点状雾查看器渲染核心 App。
class FireFogApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景静态资源（Init() 后装配一次，不在 OneIteration 里重复构造/上传）──
    uint32_t ground_mesh_ = 0;            // 40×40 地面 quad
    jpov::PBRMaterial ground_mat_;        // 灰色高粗糙地面

    // 橡树（高模 glTF）：世界摆放（center/up/front/scale），由主程序装载后传入。
    jpov::GltfObject oak_;
    jpov::Vec3f oak_center_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    jpov::Vec3f oak_up_ = jpov::Vec3f(0.0f, 1.0f, 0.0f);
    jpov::Vec3f oak_front_ = jpov::Vec3f(0.0f, 0.0f, 1.0f);
    float oak_scale_ = 1.0f;

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 光照自由度（沿用 skylight_scene.h；本查看器只固定用白昼值）──
    jpov_skylight::SkyDegrees deg_;

    // ── 点状雾参数（单个团；用户可调）──
    jpov::Vec3f fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);  // 雾体中心（橡树中段）
    float fog_radius_ = 3.0f;                                  // 半径（米）
    jpov::Color fog_color_ = jpov::Color{1.0f, 0.55f, 0.22f, 1.0f};  // 介质/发射色（暖橙）
    float fog_intensity_ = 0.6f;                               // 消光尺度 σ 系数
    int fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);

    // ── Fire-Fog 管线开关（对比用）──
    bool gaussian_enable_ = true;      // 屏幕空间高斯合并（去噪）
    bool jitter_enable_ = true;        // 段内抖动（关 → 段中点）
    float gaussian_sigma_ = 2.0f;      // 高斯 σ（低分辨率像素）；默认给到能抹平抖动的量级
    float downsample_ = 4.0f;          // ZDist 降采样倍数 N（1 = 不降采样）

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&FireFogApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 是否绘制雾（headless 拍“无雾基线”时置 false）。
    void SetShowFog(bool show) { show_fog_ = show; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        if (show_panel_) {   // 仅可见窗口交互才消费输入；headless 由外部设 view_
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

        // 相机由 view_ 推导（同 skylight viewer；target=原点）。
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 光照：白昼标准天光（单色 ambient），与 skylight viewer 同源。
        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/false);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;   // fire_fog pass 要求线性 HDR 域

        // 场景：灰色地面 + 橡树。
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(oak_, oak_center_, oak_up_, oak_front_, oak_scale_);

        // 点状雾（走 fire_fog 体积管线）。
        if (show_fog_) {
            jpov::PointFog fog;
            fog.center = fog_center_;
            fog.radius = fog_radius_;
            fog.color = fog_color_;
            fog.intensity = fog_intensity_;
            fog.attenuation = static_cast<jpov::FogAttenuation>(fog_attenuation_);
            cmds->point_fogs.push_back(fog);

            jpov::FireFogParams ff;
            ff.gaussian_enable = gaussian_enable_;
            ff.jitter_enable = jitter_enable_;
            ff.gaussian_sigma = gaussian_sigma_;
            ff.downsample = static_cast<int>(downsample_ + 0.5f);
            cmds->fire_fog = ff;
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
        FireFogApp* app = static_cast<FireFogApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float slim = 0.42f * w;         // 细滑条（数值调节）
        const float x0 = 0.02f * w;
        const float x1 = 0.52f * w;
        const float kRowH = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom = 16.0f;
        const int kRows = 10;
        const float top = h - kBottom
                        - (static_cast<float>(kRows) * kRowH
                           + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int col, int i) {
            return jpov::UiRect{
                {col == 0 ? x0 : x1, top + static_cast<float>(i) * (kRowH + kSpacing)},
                {slim, kRowH}};
        };

        // ── 左列：雾体几何/强度 ──
        ui_.SliderFloat("中心 x", &fog_center_.x(), row(0, 0), -10.0f, 10.0f, 2);
        ui_.SliderFloat("中心 y", &fog_center_.y(), row(0, 1), 0.0f, 10.0f, 2);
        ui_.SliderFloat("中心 z", &fog_center_.z(), row(0, 2), -10.0f, 10.0f, 2);
        ui_.SliderFloat("半径 m", &fog_radius_, row(0, 3), 0.2f, 10.0f, 2);
        ui_.SliderFloat("强度 σ", &fog_intensity_, row(0, 4), 0.0f, 4.0f, 3);
        {
            const std::vector<const char*> items = {
                "均匀 kUniform", "线性 kLinear", "二次 kQuadratic", "指数 kExponential"};
            ui_.Combo("衰减剖面", &fog_attenuation_, items, row(0, 5));
        }
        // ── 管线开关（对比）：高斯合并 / 抖动 / 核大小 / 降采样 ──
        ui_.Checkbox("高斯合并", &gaussian_enable_, row(0, 6));
        ui_.Checkbox("抖动", &jitter_enable_, row(0, 7));
        ui_.SliderFloat("高斯 σ(低分辨率px)", &gaussian_sigma_, row(0, 8), 0.2f, 6.0f, 1);
        ui_.SliderFloat("降采样 N", &downsample_, row(0, 9), 1.0f, 8.0f, 0);

        // ── 右列：雾色 + 天光主光仰角（便于观察雾与实体受光对比）──
        ui_.SliderFloat("色 R", &fog_color_.r, row(1, 0), 0.0f, 2.0f, 2);
        ui_.SliderFloat("色 G", &fog_color_.g, row(1, 1), 0.0f, 2.0f, 2);
        ui_.SliderFloat("色 B", &fog_color_.b, row(1, 2), 0.0f, 2.0f, 2);
        ui_.SliderFloat("太阳仰角 °", &deg_.sun_elev_deg, row(1, 3), 0.0f, 90.0f, 0);
        ui_.SliderFloat("太阳方位角 °", &deg_.sun_azim_deg, row(1, 4), 0.0f, 360.0f, 0);
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(1, 5), 0.0f, 8.0f, 1);
        ui_.ColorSwatch("雾色参考→", fog_color_, row(1, 6));
        ui_.Text("M=2段/团  K=8团/tile  tile=16px", row(1, 7));
    }

    bool show_panel_ = true;
    bool show_fog_ = true;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_fire_fog

#endif  // JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

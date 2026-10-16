// JPOV 体积火查看器（volume fire viewer）— 渲染核心 App
//
// 场景 = 灰色地面 + 一个 1m³ 燃料块（无桌子）+ 块顶的**逐像素 raymarch 体积火**。
// 目的：验证「.jpovvol 体纹理 → raymarch → 薄层写进 WBO 透明纹理 → 合成」这条链。
//
// 设计：tools/jpov/docs/jpov_volumetric_fire_design.md

#ifndef JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_
#define JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/volume_fire_types.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_volume_fire {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

class VolumeFireViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    uint32_t fuel_mesh_ = 0;                 // 1m³ 燃料块
    jpov::PBRMaterial fuel_mat_;
    uint32_t volume_id_ = 0;                  // .jpovvol 体积 id

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;
    std::string volume_path_;

    // 火焰盒（世界 AABB）：块顶 y=1.0 起，向上含整团火。
    jpov::Vec3f fire_min_ = {-0.9f, 1.0f, -0.9f};
    jpov::Vec3f fire_max_ = {0.9f, 3.2f, 0.9f};

    // 旋钮
    // A/B 基准的可复现旋钮默认值（成员初始化与命令行默认共用这一处）。
    static constexpr float kDefaultSigma = 4.0f;         // 消光 σ = 密度 × 此值 (1/m)
    static constexpr float kDefaultEmission = 1.0f;      // 发射强度
    static constexpr float kDefaultSmokeScatter = 1.0f;  // 烟散射强度
    // 烟的受光色默认（中性偏冷，含强度）；单一来源：成员初始化与 CLI 默认共用。
    static jpov::Vec3f DefaultSmokeLight() { return {0.62f, 0.66f, 0.75f}; }
    float sigma_scale_ = kDefaultSigma;
    float emission_scale_ = kDefaultEmission;
    float steps_ = 64.0f;           // 步数
    bool  jitter_ = true;           // 抖动
    float fps_ = 24.0f;             // 播放帧率（旋钮；默认值由物理推导，见注释）
    float temp_kelvin_scale_ = 2000.0f;  // G(∈[0,1]) → 开尔文
    bool  fire_enable_ = true;
    bool  fuel_enable_ = true;      // 燃料块开关（排查伪影用）
    // 烟散射强度（0 = 关）：3 通道资产的 B 通道（烟）受均匀照明散射 → 烟在纯黑底下可见。
    // 见 interface/volume_fire_types.h 的 VolumeFireCommand 注释。
    float smoke_scatter_ = kDefaultSmokeScatter;
    jpov::Vec3f smoke_light_color_ = VolumeFireViewerApp::DefaultSmokeLight();  // 烟的受光色
    bool  fs_debug_ = false;         // 调试：全屏 quad 代替盒光栅化
    float play_time_ = 0.0f;
    int   nframes_ = 64;            // 由 asset 决定（96³×64）

    // ---- A/B 基准模式（P0：钉帧 + 钉相机 + 纯黑背景）----
    // 打开后：不画地面/燃料块（只留孤立火），天光/日月/夜空归零（纯黑底），
    // 火盒 = **居中于世界原点**的立方盒（边 ab_box_m_），帧号钉死 ab_frame_。
    // 相机由调用方（RunAbCapture）钉为正面（+Z 看向原点）。
    // 目的：产出与 EmberGen 官方渲染（同为孤立火 + 黑底）可直接比对的图。
    bool  ab_mode_ = false;
    int   ab_frame_ = 0;            // 钉死的资产帧下标（>=0）
    float ab_box_m_ = 1.22f;        // A/B 火盒边长（米）；默认仅兜底，调用方按资产最大边传入

    void InstallTextMeasure() { ui_.SetTextMeasure(&VolumeFireViewerApp::TextWidth, this); }
    void SetShowPanel(bool show) { show_panel_ = show; }
    void SetShowCamInfo(bool show) { show_cam_info_ = show; }

    // 相机 + 火盒 + 关键旋钮的一行文本（屏上叠字 / 控制台打印共用）。
    std::string CamString() const {
        const jpov::Vec3f p = view_.Position();
        const double kDeg = 180.0 / 3.14159265358979323846;
        char b[640];
        std::snprintf(
            b, sizeof(b),
            "cam pos=(%.3f,%.3f,%.3f) R=%.3f phi=%.1fdeg theta=%.1fdeg | "
            "box=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) | sigma=%.2f emis=%.2f steps=%d jit=%d Tmax=%.0f frame=%.2f",
            p.x(), p.y(), p.z(), view_.R, view_.phi * kDeg, view_.theta * kDeg,
            fire_min_.x(), fire_min_.y(), fire_min_.z(),
            fire_max_.x(), fire_max_.y(), fire_max_.z(),
            sigma_scale_, emission_scale_, static_cast<int>(steps_ + 0.5f),
            jitter_ ? 1 : 0, temp_kelvin_scale_,
            std::fmod(play_time_ * fps_, static_cast<float>(nframes_)));
        return std::string(b);
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        // 按 P（或 C）→ 控制台打印当前相机信息（供截图对位/复现）。
        if (input.GetKey(jpov::KeyCode::P).IsClick() ||
            input.GetKey(jpov::KeyCode::C).IsClick()) {
            LOG(INFO) << "[camera] " << CamString();
        }
        cmds->camera.fbo_3d_width_ = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;
        if (show_panel_) {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) { dx = input.mouse_dx; dy = input.mouse_dy; }
            if (input.scroll_delta != 0.0f) { scroll = input.scroll_delta; }
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }
        cmds->camera.position = view_.Position();
        cmds->camera.target = jpov_viewer::ViewConfig::Target();
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;

        jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/false);
        if (ab_mode_) {
            // A/B 基准：天光/日月/夜空/地色全部归零 → 纯黑背景（与官方参考渲染同底）。
            MakeBlackBackground(&nl);
            // 火盒居中于世界原点（孤立火，不带地面/燃料/尺度参照）。
            const float hh = 0.5f * ab_box_m_;
            fire_min_ = {-hh, -hh, -hh};
            fire_max_ = {hh, hh, hh};
        }
        cmds->sky = nl.sky;
        cmds->sun = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 场景：地面 + 1m³ 燃料块（A/B 模式下不画——只留孤立火）。
        if (!ab_mode_) {
            cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, 0.0f, 0.0f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            if (fuel_enable_) {
                cmds->DrawObject3D(fuel_mesh_, fuel_mat_, {0.0f, 0.5f, 0.0f},
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            }
        }

        // 懒加载体积。
        if (volume_id_ == 0 && !volume_path_.empty()) {
            volume_id_ = RegisterVolumeTexture(volume_path_);
            const int nf = VolumeFrameCount(volume_id_);
            if (nf > 0) { nframes_ = nf; }
            LOG(INFO) << "[volume_fire_viewer] volume id = " << volume_id_
                      << " frames = " << nframes_ << " path = " << volume_path_;
        }

        // 体积火（逐像素 raymarch → 薄层写 WBO）。
        if (fire_enable_ && volume_id_ != 0) {
            const float dt = 1.0f / static_cast<float>(kViewerFps);
            play_time_ += dt;
            jpov::VolumeFireCommand vc;
            vc.volume_id = volume_id_;
            vc.aabb_min = fire_min_;
            vc.aabb_max = fire_max_;
            vc.sigma_scale = sigma_scale_;
            vc.emission_scale = emission_scale_;
            vc.temp_kelvin_scale = temp_kelvin_scale_;
            vc.steps = static_cast<int>(steps_ + 0.5f);
            vc.jitter = jitter_;
            vc.debug_fullscreen = fs_debug_;
            vc.smoke_scatter = smoke_scatter_;
            vc.light_color = smoke_light_color_;
            // A/B 模式：帧号钉死（可复现）；常规模式：按播放时间推进。
            vc.frame = ab_mode_ ? static_cast<float>(ab_frame_)
                                : std::fmod(play_time_ * fps_, static_cast<float>(nframes_));
            cmds->DrawVolumeFire(std::move(vc));
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
        // 相机信息叠字（右上角）—— 截图自带参数。
        if (show_cam_info_) {
            cmds->DrawText(CamString(), {static_cast<float>(kViewerWidth) - 8.0f, 8.0f},
                           13.0f, {1.0f, 1.0f, 1.0f, 1.0f},
                           jpov::TextAlignment::kTopRight, jpov::kFontBuiltinLatin);
        }
    }

private:
    // A/B 基准背景：把 SkyCommand 的天光亮度 / 日盘 / 月盘 / 夜色 / 地色全部归零，
    // 并把主平行光与环境光强度清零 → 画面只剩火自身的自发光（纯黑底）。
    // 仅 ab_mode_ 调用；常规查看器的天光不受影响。
    static void MakeBlackBackground(jpov_skylight::SkyLighting* nl /*inout*/) {
        CHECK_NOTNULL(nl);
        nl->sky.intensity = 0.0f;
        nl->sky.sun_radius = 0.0f;
        nl->sky.sun_brightness = 0.0f;
        nl->sky.sun_glow = 0.0f;
        nl->sky.moon_radius = 0.0f;
        nl->sky.moon_brightness = 0.0f;
        nl->sky.moon_glow = 0.0f;
        nl->sky.night_zenith_color = {0.0f, 0.0f, 0.0f, 1.0f};
        nl->sky.night_horizon_color = {0.0f, 0.0f, 0.0f, 1.0f};
        nl->sky.ground_color = {0.0f, 0.0f, 0.0f, 1.0f};
        nl->dir_light.intensity = 0.0f;
        nl->ambient.intensity = 0.0f;
    }

    static float TextWidth(const char* text, float font_size, const char* /*alias*/,
                           void* userdata) {
        VolumeFireViewerApp* app = static_cast<VolumeFireViewerApp*>(userdata);
        return app->MeasureTextWidth(std::string(), text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(15.0f);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);
        const float colw = 0.26f * w;
        const float cx = 0.02f * w;
        const float top = h - 16.0f - (11 * 24.0f + 10 * 5.0f);
        auto row = [&](int i) {
            return jpov::UiRect{{cx, top + i * 29.0f}, {colw, 24.0f}};
        };
        ui_.Checkbox("体积火", &fire_enable_, row(0));
        ui_.SliderFloat("消光 σ (1/m)", &sigma_scale_, row(1), 0.0f, 30.0f, 2);
        ui_.SliderFloat("发射强度", &emission_scale_, row(2), 0.0f, 5.0f, 2);
        ui_.SliderFloat("步数", &steps_, row(3), 8.0f, 256.0f, 0);
        ui_.Checkbox("抖动", &jitter_, row(4));
        ui_.SliderFloat("帧率 fps", &fps_, row(5), 4.0f, 60.0f, 0);
        ui_.SliderFloat("温度上限 K", &temp_kelvin_scale_, row(6), 800.0f, 4000.0f, 0);
        ui_.Checkbox("相机信息", &show_cam_info_, row(7));
        ui_.Checkbox("燃料块", &fuel_enable_, row(8));
        ui_.Checkbox("全屏quad(调试)", &fs_debug_, row(9));
        ui_.SliderFloat("烟散射", &smoke_scatter_, row(10), 0.0f, 20.0f, 2);
    }

    bool show_panel_ = true;
    bool show_cam_info_ = true;
    jpov::Ui ui_;
};

}  // namespace jpov_volume_fire

#endif  // JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_

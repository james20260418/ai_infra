// JPOV 雾火查看器（fire_fog viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 目的：**肉眼验收 froxel 体积雾 / 火**（设计见 tools/jpov/docs/jpov_froxel_design.md）的最小测试场。
// 场景 = 灰色地面 + 一棵高模橡树（oak），雾/火默认摆在橡树处（便于观察与实体的深度关系：
// 火在树前正常叠加，树前的火被场景深度裁剪，树后的火被树遮挡）。
//
// 与 skylight viewer 同构（同一套 JPOV::OneIteration 渲染体 + 即时模式 UI 面板约定）。
//
// ── 面板布局（Danis 2026-10-11 约定）──
//   · **顶栏**：`团类型` 下拉（round box / flame）。
//   · **左下**：**per-团类型** 调参面板 —— 团类型切换时整块跟着换（各型一套滑块）。
//   · **右下**：**光照** 面板 —— **全局共享**（点光源 / 太阳 / god ray / froxel 分辨率），不随团类型变。
//
// ⚠️ 雾火**始终走物理光照**（ambient + 太阳×CSM + 点光源）；「无光照」链路已移除。
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + 右键拖拽/滚轮缩放）。

#ifndef JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_
#define JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

#include <algorithm>
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

// froxel 分辨率可选档（与 FireFogParams.nz / tile_px 对应，面板下拉的候选值）。
// nz 必须是完全平方（见 FireFogParams 约束）。
inline constexpr int kNzChoices[] = {64, 256};
inline constexpr int kTilePxChoices[] = {8, 16, 32};

// 团类型（顶栏下拉；决定左下角显示哪套 per-团面板、以及本帧发射哪种子 command）。
enum class FogKindChoice : int {
    kRoundBox = 0,   // 圆角盒雾（PointFog）
    kFlame = 1,      // 燃料盒包裹燃烧（FlameFog，MVP：只有「形」，无 3D 纹理）
};

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

    // ── 团类型（顶栏下拉；索引 = FogKindChoice）──
    int fog_kind_index_ = static_cast<int>(FogKindChoice::kFlame);

    // ── 圆角盒雾参数（团类型 = kRoundBox 时生效）──
    jpov::Vec3f fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);  // box 空间位置（橡树中段）
    jpov::Vec3f fog_size_ = jpov::Vec3f(6.0f, 6.0f, 6.0f);    // box 长宽高（米）
    float fog_radius_ = 1.0f;                                  // box 外侧圆角壳厚度（米）
    float fog_sigma_ = 0.6f;                                   // 消光系数 σ_t（1/m）
    jpov::Color fog_albedo_ = jpov::Color{0.9f, 0.9f, 0.9f, 1.0f};    // 散射色（∈[0,1]）
    jpov::Color fog_emission_ = jpov::Color{0.0f, 0.0f, 0.0f, 1.0f};  // 自发光 ε（HDR，rad/m）
    int fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);

    // ── 火焰参数（团类型 = kFlame 时生效）──
    jpov::Vec3f flame_center_ = jpov::Vec3f(0.0f, 1.5f, 0.0f);  // 燃料盒中心
    jpov::Vec3f flame_size_ = jpov::Vec3f(3.0f, 3.0f, 3.0f);    // 燃料盒长宽高（米）
    jpov::Vec3f flame_up_ = jpov::Vec3f(0.0f, 1.0f, 0.0f);      // 火苗主方向（默认朝上）
    float flame_shell_ = 0.6f;                                   // 包裹壳厚（米）
    float flame_plume_ = 3.0f;                                   // 羽流段长（米）
    float flame_vigor_ = 3.0f;                                   // 旺盛度（自发光放大）
    float flame_sigma_ = 1.2f;                                   // 消光系数 σ_t（1/m）
    jpov::Color flame_albedo_ = jpov::Color{0.6f, 0.25f, 0.1f, 1.0f};   // 散射色（∈[0,1]）
    jpov::Color flame_emission_ = jpov::Color{1.0f, 0.45f, 0.12f, 1.0f}; // 自发光颜色（HDR，rad/m）

    // ── 一盏点光源（演示 fire_fog 消费点光源：自持 Nxy tile culling）──
    bool point_light_enable_ = true;
    jpov::Vec3f point_light_pos_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    jpov::Color point_light_color_ = jpov::Color{1.0f, 0.85f, 0.6f, 1.0f};
    float point_light_intensity_ = 8.0f;
    float point_light_radius_ = 12.0f;

    // ── god ray ──
    float sun_phase_g_ = 0.7f;         // HG 相位各向异性（太阳/点光源共用）
    float sun_gain_ = 2.0f;            // 太阳项增益

    // ── froxel 分辨率（面板下拉；对应 FireFogParams.nz / tile_px）──
    int nz_index_ = 1;        // 0 → Nz=64，1 → Nz=256
    int tile_px_index_ = 1;   // 0 → 8px，1 → 16px，2 → 32px
    // ── froxel z 分布远端（米）；近端固定 0.1（FireFogParams.z_near 默认）。──
    float z_far_ = 2000.0f;

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&FireFogApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 是否绘制雾/火（headless 拍“无雾基线”时置 false）。
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

        // 点光源（fire_fog 消费：自持 Nxy tile culling）——**独立于雾**，稳定场景光照。
        if (point_light_enable_) {
            jpov::PointLight pl;
            pl.position = point_light_pos_;
            pl.color = point_light_color_;
            pl.linear_radius = point_light_radius_;
            pl.intensity = point_light_intensity_;
            cmds->point_lights.push_back(pl);
        }

        // 雾/火（走 fire_fog froxel 体积管线）：按团类型发射对应子 command。
        if (show_fog_) {
            if (fog_kind_index_ == static_cast<int>(FogKindChoice::kRoundBox)) {
                jpov::PointFog fog;
                fog.center = fog_center_;
                fog.size = fog_size_;
                fog.radius = fog_radius_;
                fog.sigma = fog_sigma_;
                fog.albedo = fog_albedo_;
                fog.emission = fog_emission_;
                fog.attenuation = static_cast<jpov::FogAttenuation>(fog_attenuation_);
                cmds->point_fogs.push_back(fog);
            } else {
                jpov::FlameFog ff;
                ff.center = flame_center_;
                ff.size = flame_size_;
                ff.up = flame_up_;
                ff.shell = flame_shell_;
                ff.plume = flame_plume_;
                ff.vigor = flame_vigor_;
                ff.sigma = flame_sigma_;
                ff.albedo = flame_albedo_;
                ff.emission = flame_emission_;
                cmds->flame_fogs.push_back(ff);
            }

            jpov::FireFogParams params;
            params.sun_phase_g = sun_phase_g_;
            params.sun_gain = sun_gain_;
            params.nz = kNzChoices[std::min(std::max(nz_index_, 0), 1)];
            params.tile_px = kTilePxChoices[std::min(std::max(tile_px_index_, 0), 2)];
            params.z_far = z_far_;
            cmds->fire_fog = params;
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

        // ── 布局常量 ──
        const float kRowH = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom = 16.0f;
        const float colw = 0.22f * w;
        const float gap = 8.0f;
        const float right_x = w - (2.0f * colw + gap) - 20.0f;

        // 底部锚定的网格单元：给定面板左上 x / 行数 / 列 / 行 → 控件矩形。
        auto cell = [&](float x0, int nrows, int col, int i) -> jpov::UiRect {
            const float top = h - kBottom
                            - (static_cast<float>(nrows) * kRowH
                               + static_cast<float>(nrows - 1) * kSpacing);
            const float x = x0 + static_cast<float>(col) * (colw + gap);
            return jpov::UiRect{{x, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {colw, kRowH}};
        };

        // ── 顶栏：团类型下拉 ──
        {
            const std::vector<const char*> items = {"圆角盒 RoundBox", "火焰 Flame"};
            ui_.Combo("团类型", &fog_kind_index_, items, jpov::UiRect{{20.0f, 12.0f}, {300.0f, 28.0f}});
        }

        // ── 左下：per-团类型 面板 ──
        if (fog_kind_index_ == static_cast<int>(FogKindChoice::kRoundBox)) {
            DrawRoundBoxPanel(cell);
        } else {
            DrawFlamePanel(cell);
        }

        // ── 右下：光照（全局共享）──
        DrawLightingPanel(cell, right_x);
    }

    // 左下：圆角盒雾 per-团面板（8 行 × 2 列）。
    template <typename CellFn>
    void DrawRoundBoxPanel(const CellFn& cell) {
        const float x0 = 20.0f;
        const int kRows = 8;
        ui_.SliderFloat("中心 x", &fog_center_.x(), cell(x0, kRows, 0, 0), -10.0f, 10.0f, 2);
        ui_.SliderFloat("中心 y", &fog_center_.y(), cell(x0, kRows, 0, 1), 0.0f, 10.0f, 2);
        ui_.SliderFloat("中心 z", &fog_center_.z(), cell(x0, kRows, 0, 2), -10.0f, 10.0f, 2);
        ui_.SliderFloat("尺寸 x m", &fog_size_.x(), cell(x0, kRows, 0, 3), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 y m", &fog_size_.y(), cell(x0, kRows, 0, 4), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 z m", &fog_size_.z(), cell(x0, kRows, 0, 5), 0.2f, 12.0f, 2);
        ui_.SliderFloat("圆角半径 m", &fog_radius_, cell(x0, kRows, 0, 6), 0.0f, 6.0f, 2);
        ui_.SliderFloat("消光 σ 1/m", &fog_sigma_, cell(x0, kRows, 0, 7), 0.0f, 4.0f, 3);
        {
            const std::vector<const char*> items = {
                "均匀 kUniform", "线性 kLinear", "二次 kQuadratic", "指数 kExponential"};
            ui_.Combo("衰减剖面", &fog_attenuation_, items, cell(x0, kRows, 1, 0));
        }
        ui_.SliderFloat("散射 R", &fog_albedo_.r, cell(x0, kRows, 1, 1), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 G", &fog_albedo_.g, cell(x0, kRows, 1, 2), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 B", &fog_albedo_.b, cell(x0, kRows, 1, 3), 0.0f, 1.0f, 2);
        ui_.SliderFloat("自发光 R", &fog_emission_.r, cell(x0, kRows, 1, 4), 0.0f, 4.0f, 2);
        ui_.SliderFloat("自发光 G", &fog_emission_.g, cell(x0, kRows, 1, 5), 0.0f, 4.0f, 2);
        ui_.SliderFloat("自发光 B", &fog_emission_.b, cell(x0, kRows, 1, 6), 0.0f, 4.0f, 2);
    }

    // 左下：火焰 per-团面板（10 行 × 2 列）。
    template <typename CellFn>
    void DrawFlamePanel(const CellFn& cell) {
        const float x0 = 20.0f;
        const int kRows = 10;
        ui_.SliderFloat("中心 x", &flame_center_.x(), cell(x0, kRows, 0, 0), -10.0f, 10.0f, 2);
        ui_.SliderFloat("中心 y", &flame_center_.y(), cell(x0, kRows, 0, 1), 0.0f, 10.0f, 2);
        ui_.SliderFloat("中心 z", &flame_center_.z(), cell(x0, kRows, 0, 2), -10.0f, 10.0f, 2);
        ui_.SliderFloat("尺寸 x m", &flame_size_.x(), cell(x0, kRows, 0, 3), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 y m", &flame_size_.y(), cell(x0, kRows, 0, 4), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 z m", &flame_size_.z(), cell(x0, kRows, 0, 5), 0.2f, 12.0f, 2);
        ui_.SliderFloat("火苗 x", &flame_up_.x(), cell(x0, kRows, 0, 6), -1.0f, 1.0f, 2);
        ui_.SliderFloat("火苗 y", &flame_up_.y(), cell(x0, kRows, 0, 7), -1.0f, 1.0f, 2);
        ui_.SliderFloat("火苗 z", &flame_up_.z(), cell(x0, kRows, 0, 8), -1.0f, 1.0f, 2);
        ui_.SliderFloat("壳厚 m", &flame_shell_, cell(x0, kRows, 0, 9), 0.05f, 3.0f, 2);
        ui_.SliderFloat("羽流高 m", &flame_plume_, cell(x0, kRows, 1, 0), 0.0f, 10.0f, 2);
        ui_.SliderFloat("旺盛度", &flame_vigor_, cell(x0, kRows, 1, 1), 0.0f, 8.0f, 2);
        ui_.SliderFloat("消光 σ 1/m", &flame_sigma_, cell(x0, kRows, 1, 2), 0.0f, 4.0f, 3);
        ui_.SliderFloat("散射 R", &flame_albedo_.r, cell(x0, kRows, 1, 3), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 G", &flame_albedo_.g, cell(x0, kRows, 1, 4), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 B", &flame_albedo_.b, cell(x0, kRows, 1, 5), 0.0f, 1.0f, 2);
        ui_.SliderFloat("自发光 R", &flame_emission_.r, cell(x0, kRows, 1, 6), 0.0f, 8.0f, 2);
        ui_.SliderFloat("自发光 G", &flame_emission_.g, cell(x0, kRows, 1, 7), 0.0f, 8.0f, 2);
        ui_.SliderFloat("自发光 B", &flame_emission_.b, cell(x0, kRows, 1, 8), 0.0f, 8.0f, 2);
    }

    // 右下：光照面板（全局共享；7 行 × 2 列）。
    template <typename CellFn>
    void DrawLightingPanel(const CellFn& cell, float x0) {
        const int kRows = 7;
        ui_.Checkbox("点光源", &point_light_enable_, cell(x0, kRows, 0, 0));
        ui_.SliderFloat("点光 x", &point_light_pos_.x(), cell(x0, kRows, 0, 1), -10.0f, 10.0f, 2);
        ui_.SliderFloat("点光 y", &point_light_pos_.y(), cell(x0, kRows, 0, 2), 0.0f, 10.0f, 2);
        ui_.SliderFloat("点光 z", &point_light_pos_.z(), cell(x0, kRows, 0, 3), -10.0f, 10.0f, 2);
        ui_.SliderFloat("点光 亮度", &point_light_intensity_, cell(x0, kRows, 0, 4), 0.0f, 40.0f, 1);
        ui_.SliderFloat("点光 半径", &point_light_radius_, cell(x0, kRows, 0, 5), 0.5f, 40.0f, 1);
        ui_.SliderFloat("太阳仰角 °", &deg_.sun_elev_deg, cell(x0, kRows, 0, 6), 0.0f, 90.0f, 0);
        ui_.SliderFloat("太阳方位角 °", &deg_.sun_azim_deg, cell(x0, kRows, 1, 0), 0.0f, 360.0f, 0);
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, cell(x0, kRows, 1, 1), 0.0f, 8.0f, 1);
        ui_.SliderFloat("光柱 相位 g", &sun_phase_g_, cell(x0, kRows, 1, 2), 0.0f, 0.9f, 2);
        ui_.SliderFloat("光柱 增益", &sun_gain_, cell(x0, kRows, 1, 3), 0.0f, 4.0f, 2);
        {
            const std::vector<const char*> items = {"Nz=64", "Nz=256"};
            ui_.Combo("z 切片 Nz", &nz_index_, items, cell(x0, kRows, 1, 4));
        }
        {
            const std::vector<const char*> items = {"tile=8px", "tile=16px", "tile=32px"};
            ui_.Combo("Nxy 单元 tile", &tile_px_index_, items, cell(x0, kRows, 1, 5));
        }
        ui_.SliderFloat("z 远端 far(m)", &z_far_, cell(x0, kRows, 1, 6), 50.0f, 4000.0f, 0);
    }

    bool show_panel_ = true;
    bool show_fog_ = true;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_fire_fog

#endif  // JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

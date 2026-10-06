// JPOV 雾火查看器（fire_fog viewer）— 渲染核心 App（雾火调试宿主）
//
// 以 skylight viewer 为基础，**专门调试雾火（fire_fog）**。相对 skylight viewer 的差别：
//   - 场景**精简**：方块 3→1（留中间一个）、蓝人 3→1（留中间方块顶面一个）；地面同；
//   - 新增**点状雾体**（fire_fog 管线的用户级输入）：默认在屏幕中心放一团 2 米的点雾；
//   - 新增**调试开关**：把「有雾火元素覆盖的屏幕 tile」涂一层 color blend，肉眼核对
//     L1 tile 剪枝（见 docs/jpov_fire_fog_design.md §3 / §10.5）。
// 其余（相机手感 / 天光自由度 / 远景仰角雾面板）与 skylight viewer 保持一致。
//
// 复用：场景几何/材质/天光装配直接用 jpov_skylight::（skylight_scene.h）；视角用 ViewConfig。

#ifndef JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_
#define JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"   // 场景几何/材质/天光装配（复用）
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"       // kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/ui.h"

namespace jpov_fire_fog {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 雾火查看器渲染核心 App。
class FireFogApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景静态资源（Init() 后装配一次）──
    uint32_t box_mesh_ = 0;               // 1×1×1 方块（中间那一个）
    uint32_t ground_mesh_ = 0;            // 40×40 地面 quad
    jpov::PBRMaterial box_material_;      // 中间方块的材质（skylight 的“高光”块）
    jpov::PBRMaterial ground_material_;   // 灰色地面

    // ── 蓝人（mixamo_male）：一份蒙皮 mesh + 骨架，中间方块顶面一个 T-pose 实例 ──
    uint32_t person_mesh_ = 0;
    uint32_t person_skeleton_id_ = 0;
    jpov::PBRMaterial person_material_;
    jpov::GltfObject person_gltf_;
    std::vector<jpov::SkinnedInstanceState> person_instances_;

    // ── 额外模型（桌子 / 高模橡树），与 skylight viewer 同（用于观察物体受光）──
    struct ModelSlot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
        float scale = 1.0f;
    };
    std::vector<ModelSlot> models_;

    void AddModel(jpov::GltfObject obj, jpov::Vec3f center,
                  jpov::Vec3f up, jpov::Vec3f front, float scale = 1.0f) {
        models_.push_back({std::move(obj), center, up, front, scale});
    }

    // ── 视角 ──
    jpov_viewer::ViewConfig view_;

    // ── 天光自由度（与 skylight viewer 同）──
    jpov_skylight::SkyDegrees deg_;

    // ── 点状雾体（fire_fog）调试参数 ──
    bool  fog_on_ = true;                 // 是否放这团点雾
    float fog_radius_ = 2.0f;             // 半径（米）
    float fog_center_y_ = 1.0f;           // 中心高度（米）——屏幕中心附近
    bool  fog_debug_tiles_ = true;        // 把有元素的 tile 涂色（验证 L1 剪枝）

    void SetTricolorAmbient(bool on) { tricolor_ambient_ = on; }
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&FireFogApp::AppTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }
    void SetShowScene(bool show) { show_scene_ = show; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（零分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

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
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 天光 + 光照：全由自由度推导（同 skylight viewer）。
        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, tricolor_ambient_);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 远景仰角雾（空气透视，同 skylight viewer）。
        if (deg_.elev_fog_on) {
            const jpov::Color fc = nl.sky.ElevationFogColor();
            jpov::ElevationFogConfig ef;
            ef.enabled = true;
            ef.start_distance = deg_.elev_fog_start;
            ef.full_distance = deg_.elev_fog_full;
            ef.elev_inner_deg = deg_.elev_fog_inner_deg;
            ef.elev_outer_deg = deg_.elev_fog_outer_deg;
            ef.density = deg_.elev_fog_density;
            ef.use_sky_color = deg_.elev_fog_use_sky;
            ef.color = {fc.r * deg_.elev_fog_gain, fc.g * deg_.elev_fog_gain,
                        fc.b * deg_.elev_fog_gain, 1.0f};
            cmds->elevation_fog = ef;
        }

        // 场景：灰色地面 + 中间一个方块（+ 桌子/橡树 + 中间一个蓝人）。
        if (show_scene_) {
            cmds->DrawObject3D(ground_mesh_, ground_material_,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f});
            // 只留中间一个方块（skylight viewer 的第 1 号）。
            cmds->DrawObject3D(box_mesh_, box_material_, jpov_skylight::BoxCenter(1),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            for (const ModelSlot& m : models_) {
                cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
            }
            if (person_mesh_ != 0 && !person_instances_.empty()) {
                cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                           person_material_, person_instances_);
            }
        }

        // 点状雾体（fire_fog 输入）：屏幕中心一团 2 米的点雾 + tile 剪枝调试。
        if (fog_on_) {
            jpov::PointFog pf;
            pf.center = {0.0f, fog_center_y_, 0.0f};
            pf.radius = fog_radius_;
            pf.color = {0.9f, 0.9f, 0.9f, 1.0f};
            pf.intensity = 1.0f;
            pf.attenuation = jpov::FogAttenuation::kQuadratic;
            cmds->point_fogs.push_back(pf);
        }
        cmds->debug_fire_fog_tiles = fog_debug_tiles_;

        // 面板（仅交互窗口）。
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

    // 三列面板：左=天光自由度；中=远景仰角雾；右=雾火（点雾 + tile 调试）。
    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kColW    = 0.30f * w;
        const float kColX[3] = {0.02f * w, 0.35f * w, 0.68f * w};
        const float kRowH    = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom  = 16.0f;
        const int   kRows    = 9;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int col, int i) {
            return jpov::UiRect{{kColX[col], top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kColW, kRowH}};
        };

        // ── 左列：天光自由度（同 skylight viewer）──
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(0, 0),
                        jpov_skylight::kTurbidityMin, jpov_skylight::kTurbidityMax, 1);
        ui_.SliderFloat("季节色温 日光 (左蓝..右红)", &deg_.daylight_season,
                        row(0, 1), -1.0f, 1.0f, 2);
        ui_.SliderFloat("天体仰角 ° (负=月/正=日)", &deg_.sun_elev_deg,
                        row(0, 2), -90.0f, 90.0f, 0);
        ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(0, 3), 0.0f, 360.0f, 0);
        ui_.SliderFloat("月色变红 (血月)", &deg_.moon_red, row(0, 4), 0.0f, 1.0f, 2);
        ui_.SliderFloat("夜空偏蓝 (梦幻夜)", &deg_.night_blue, row(0, 5), 0.0f, 1.0f, 2);
        ui_.Checkbox("三色环境光 (天/天际线/地)", &tricolor_ambient_, row(0, 6));

        // ── 中列：远景仰角雾（同 skylight viewer）──
        ui_.Checkbox("远景仰角雾 (空气透视)", &deg_.elev_fog_on, row(1, 0));
        ui_.SliderFloat("起雾距离 m", &deg_.elev_fog_start, row(1, 1), 0.0f, 2000.0f, 2);
        ui_.SliderFloat("满雾距离 m", &deg_.elev_fog_full, row(1, 2), 1.0f, 5000.0f, 1);
        ui_.SliderFloat("仰角带内 °", &deg_.elev_fog_inner_deg, row(1, 3), 0.0f, 20.0f, 1);
        ui_.SliderFloat("仰角带外 °", &deg_.elev_fog_outer_deg, row(1, 4), 0.1f, 40.0f, 1);
        ui_.SliderFloat("雾浓度 σ", &deg_.elev_fog_density, row(1, 5), 0.0f, 8.0f, 2);
        ui_.SliderFloat("雾色增益", &deg_.elev_fog_gain, row(1, 6), 0.0f, 5.0f, 2);
        ui_.Checkbox("雾色跟随天空", &deg_.elev_fog_use_sky, row(1, 7));

        // ── 右列：雾火（点雾 + tile 剪枝调试）──
        ui_.Checkbox("点状雾 (fire_fog)", &fog_on_, row(2, 0));
        ui_.SliderFloat("雾半径 m", &fog_radius_, row(2, 1), 0.1f, 10.0f, 2);
        ui_.SliderFloat("雾中心高度 y", &fog_center_y_, row(2, 2), -2.0f, 6.0f, 2);
        ui_.Checkbox("显示有元素的 tile (调试)", &fog_debug_tiles_, row(2, 3));
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    bool tricolor_ambient_ = false;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_fire_fog

#endif  // JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

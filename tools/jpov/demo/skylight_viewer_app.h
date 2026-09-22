// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照来源**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 天光 = `CreateDefaultSkyCommand(四自由度)`（其余参数走默认构造）；
//   - 光照 = 由该 SkyCommand **推导**（日光色/强 + ambient 色/强），不手配。
//
// 交互面板只有**四个自由度**（六个滑条；Danis 2026-09-21 定）：
//   ① 浊度 turb [0,8]
//   ② 季节色温 [−1,+1]（左=蓝偏 / 右=红偏；这是查看器的交互设计，见 scene 头注释）
//   ③ 太阳方向：仰角 [0,90] + 方位角 [0,360)（两个滑条，属同一个自由度）
//   ④ 月亮方向：仰角 [0,90] + 方位角 [0,360)
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + 右键拖拽/滚轮缩放），
// 保证两个查看器手感一致；默认相机放在三方块斜前方，一眼看全三块。

#ifndef JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_
#define JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"   // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/ui.h"

namespace jpov_skylight {

// 分辨率/帧率/字体别名沿用 model viewer 的单点定义（viewer_app.h），
// 避免两个查看器各写一套硬编码而分叉。
using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 天光查看器渲染核心 App。
class SkylightApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景静态资源（Init() 后装配一次，不在 OneIteration 里重复构造/上传）──
    uint32_t box_mesh_ = 0;              // 1×1×1 方块（三块共用）
    uint32_t ground_mesh_ = 0;           // 40×40 地面 quad
    jpov::PBRMaterial mat_low_;          // 低反（rough=1.0, metal=0）
    jpov::PBRMaterial mat_high_;         // 高光（rough=0.05, metal=0）
    jpov::PBRMaterial mat_metal_;        // 金属（metal=1, rough=0.15）
    jpov::PBRMaterial mat_ground_;       // 灰色地面

    // ── 额外模型（桌子 / 高模橡树）：用来看“物体受光”，供标定夜色 ambient ──
    // 每个 Slot = 一个 glTF + 世界摆放（center/up/front/scale），由主程序装载后 AddModel。
    struct ModelSlot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
        float scale = 1.0f;   // 整体缩放（资产原始尺寸差异很大，需按目标高度归一）
    };
    std::vector<ModelSlot> models_;

    void AddModel(jpov::GltfObject obj, jpov::Vec3f center,
                  jpov::Vec3f up, jpov::Vec3f front, float scale = 1.0f) {
        models_.push_back({std::move(obj), center, up, front, scale});
    }

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 四个自由度（跨帧持有；天光与光照全由它们推导）──
    float turbidity_    = kTurbidityDef;   // ① 浊度 [0,8]
    float season_tint_  = 0.0f;            // ② 季节色温 [−1,+1]（蓝偏..红偏，0=中性）
    float sun_elev_deg_ = 45.0f;           // ③ 太阳仰角 [0,90]
    float sun_azim_deg_ = 45.0f;           // ③ 太阳方位角 [0,360)
    float moon_elev_deg_ = 30.0f;          // ④ 月亮仰角 [0,90]
    float moon_azim_deg_ = 225.0f;         // ④ 月亮方位角 [0,360)

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SkylightApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制光照面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 是否绘制场景几何（三方块 + 地面）。headless 拍“天空本身”时置 false，
    // 排除方块遮挡与受光干扰，只留天光背景。
    void SetShowScene(bool show) { show_scene_ = show; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 改视角（同 model viewer 的 ApplyInput）；headless 不消费输入。
        if (show_panel_) {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }

        // 相机由 view_ 推导（同 model viewer；target=原点）。
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 天光 + 光照：全由四个自由度推导（sky 走 CreateDefaultSkyCommand，
        // 日光/ambient 由该 sky 推导）；夜色两个标定旋钮覆盖默认值。
        SkyLighting nl = MakeSkyLighting(
            turbidity_, season_tint_, sun_elev_deg_, sun_azim_deg_,
            moon_elev_deg_, moon_azim_deg_);
        // 光照由该 sky 推导（日光/ambient 全部由 SkyCommand 推出，不手配）。
        nl.sun = jpov::DirectionalLight{
            /*direction*/ {-nl.sky.sun_dir.x(), -nl.sky.sun_dir.y(), -nl.sky.sun_dir.z()},
            /*color*/ nl.sky.SunDirectionalColor(),
            /*intensity*/ nl.sky.SunDirectionalIntensity(),
        };
        nl.ambient = jpov::AmbientLight{
            .color = nl.sky.AmbientColor(),
            .intensity = nl.sky.AmbientIntensity(),
        };
        cmds->sky     = nl.sky;
        cmds->sun     = nl.sun;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 场景：灰色地面 + 三方块（低反/高光/金属）。
        // show_scene_=false 时整组跳过（headless 拍纯天空用）。
        if (show_scene_) {
            cmds->DrawObject3D(ground_mesh_, mat_ground_,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_low_,   BoxCenter(0),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_high_,  BoxCenter(1),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_metal_, BoxCenter(2),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            // 额外模型（桌子 / 橡树）：用来看“物体受光”，供夜色标定。
            for (const ModelSlot& m : models_) {
                cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
            }
        }

        // 光照面板（仅交互窗口；headless 拍摄是纯 3D 截图）。
        if (show_panel_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        SkylightApp* app = static_cast<SkylightApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 光照面板：6 个滑条 = 四个自由度（日月各占仰角+方位角）。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kSliderWidth = 0.5f * w;
        const float kRowH    = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom  = 16.0f;
        const int   kRows    = 6;
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };

        // ① 浊度 [0,8]：看霾化（天色发白）+ 日盘/月盘衰减 + 月晕变宽。
        ui_.SliderFloat("浊度 turb", &turbidity_, row(0),
                        kTurbidityMin, kTurbidityMax, 1);
        // ② 季节色温 [−1,+1]：左蓝偏 / 右红偏 / 中间中性（只偏色温，不改亮度）。
        ui_.SliderFloat("季节色温 (左蓝偏..右红偏)", &season_tint_, row(1),
                        -1.0f, 1.0f, 2);
        // ③ 太阳方向（仰角 + 方位角）。
        ui_.SliderFloat("太阳仰角 °", &sun_elev_deg_, row(2), 0.0f, 90.0f, 0);
        ui_.SliderFloat("太阳方位角 °", &sun_azim_deg_, row(3), 0.0f, 360.0f, 0);
        // ④ 月亮方向（仰角 + 方位角）。月盘亮度走默认 5（常亮）。
        ui_.SliderFloat("月亮仰角 °", &moon_elev_deg_, row(4), 0.0f, 90.0f, 0);
        ui_.SliderFloat("月亮方位角 °", &moon_azim_deg_, row(5), 0.0f, 360.0f, 0);
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

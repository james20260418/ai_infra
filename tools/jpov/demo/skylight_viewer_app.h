// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照参数**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 光照 = skylight_scene.h 的 MakeSky/MakeSun/MakeAmbient，滑条里多了
//     「天光强度（太阳）/ 环境光强度 / 夜色强度」三个绝对量旋钮。
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + ApplyInput 右键拖拽/
// 滚轮缩放），保证两个查看器手感一致；默认相机放在三方块斜前方，一眼看全三块。

#ifndef JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_
#define JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

#include <string>

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

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 光照滑条状态（跨帧持有；色调由 sky 推导，亮度由滑条直接给）──
    float elev_deg_    = 20.0f;   // 太阳仰角 [0,90]：0=日落（纯夜色），90=正午
    float turbidity_   = 2.0f;    // 大气浊度 [2,8]
    float season_r_    = 1.0f;    // 季节 R 色温乘子 [0.5,2.0]（只染白天项）
    float sun_intensity_     = 3.0f;   // 太阳平行光强度（绝对值；0=无直射）
    float ambient_intensity_ = 0.3f;   // 环境光强度（绝对值；0=无 ambient）
    float night_scale_ = 1.0f;    // 夜色强度乘子 [0,4]（只乘夜色两色；0=关夜色）

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SkylightApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制光照面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

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

        // 光照：sky（含夜色两色 × 夜色强度）+ 由 sky 推导色调的 sun/ambient。
        const jpov::SkyCommand sky =
            MakeSky(elev_deg_, turbidity_, season_r_, night_scale_);
        cmds->sky     = sky;
        cmds->sun     = MakeSun(sky, sun_intensity_);
        cmds->ambient = MakeAmbient(sky, ambient_intensity_);
        cmds->tone_mapping = true;

        // 场景：灰色地面 + 三方块（低反/高光/金属）。
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

    // 光照面板：6 个滑条（仰角 / 浊度 / 季节 R / 太阳强度 / 环境光强度 / 夜色强度）。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kSliderWidth = 0.5f * w;
        const float kRowH    = 28.0f;
        const float kSpacing = 8.0f;
        const float kBottom  = 16.0f;
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom - (6.0f * kRowH + 5.0f * kSpacing);

        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };

        // 太阳仰角 0~90：左端 0° = 日落（白天项被压到 0，留纯夜色），右端正午。
        ui_.SliderFloat("太阳仰角 °", &elev_deg_, row(0), 0.0f, 90.0f, 0);
        // 浊度 [2,8]：看霾化 + Turb*Loss 强度衰减。
        ui_.SliderFloat("浊度 turb", &turbidity_, row(1), 2.0f, 8.0f, 1);
        // 季节 R [0.5,2.0]：只染白天项，拉极端可验证"夜色不被季节染色"。
        ui_.SliderFloat("季节 R", &season_r_, row(2), 0.5f, 2.0f, 2);
        // 太阳/环境光强度是**绝对量**（滑条所见即所得，不走 PWL 相对曲线）。
        ui_.SliderFloat("太阳强度", &sun_intensity_, row(3), 0.0f, 10.0f, 2);
        ui_.SliderFloat("环境光强度", &ambient_intensity_, row(4), 0.0f, 2.0f, 2);
        // 夜色强度 [0,4]：独立乘夜色两色（0=关夜色，用于对照无夜色画面）。
        ui_.SliderFloat("夜色强度", &night_scale_, row(5), 0.0f, 4.0f, 2);
    }

    bool show_panel_ = true;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照来源**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 天光 = `CreateDefaultSkyCommand`（其余参数走默认构造；月亮恒取 moon_dir = −sun_dir）；
//   - 光照 = 由该 SkyCommand **推导**（主平行光 + ambient 色/强），不手配。
//
// 交互面板有五个自由度（六个滑条）：
//   ① 浊度 turb [0,8]
//   ② 季节色温 日光 [−1,+1]（左=蓝偏 / 右=红偏；只染太阳能通道）
//   ③ 天体方向：仰角 [−90,+90] + 方位角 [0,360)（两个滑条，属同一个自由度）
//      仰角正 = 太阳在地平线上（白天，太阳平行光）；负 = 太阳沉下、月亮升到反向
//      等高（夜间，月亮平行光）。月亮方向恒取 moon_dir = −sun_dir，不单独给滑条。
//   ④ 月色变红 [0,1]（0=常月，1=血月；只染月盘 + 月光）
//   ⑤ 夜空偏蓝 [0,1]（0=出厂夜色，1=梦幻蓝且更亮；夜色两色整体乘子）
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

    // ── 天光自由度（跨帧持有；天光与光照全由它们推导）──
    // 滑条值 → SkyCommand 字段的编码见 skylight_scene.h 的 SkyDegrees。
    SkyDegrees deg_;

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SkylightApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制光照面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 是否绘制场景几何（三方块 + 地面）。headless 拍“天空本身”时置 false，
    // 排除方块遮挡与受光干扰，只留天光背景。
    void SetShowScene(bool show) { show_scene_ = show; }

    // 是否绘制实验性火焰（含其上方的暖色点光源）。
    // 交互面板有一个 toggle；headless 拍摄可用本 setter 单独开/关出图对比。
    void SetShowFire(bool show) { show_fire_ = show; }
    bool show_fire() const { return show_fire_; }

    // 覆盖特效时钟（秒）。headless 拍摄用：不推进帧计数器，也能拍到动画中段。
    // 传 0 生效；传负值恢复“跟随帧计数器”（交互默认）。
    void SetEffectTime(float t) { effect_time_override_ = t; }

    // 火焰混合模式（加法/alpha），供拍摄对比用。
    void SetFireBlend(jpov::ParticleBlend b) { fire_blend_ = b; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 特效时钟：默认由帧计数器推进（确定性，不用 wall-clock）；
        // headless 拍摄可用 SetEffectTime 覆盖以拍到动画中段。
        if (effect_time_override_ >= 0.0f) {
            cmds->effect_time = effect_time_override_;
        } else {
            cmds->effect_time = static_cast<float>(frame_count) /
                                static_cast<float>(kViewerFps);
        }

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

        // 天光 + 光照：全由五个自由度推导——sky 走 CreateDefaultSkyCommand，
        // 主平行光（白天太阳/夜间月亮）与 ambient 全由该 sky 推导（含夜色项）；
        // moon_season 与夜空蓝覆盖默认值。
        const SkyLighting nl = MakeSkyLighting(deg_);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
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

        // 火焰（实验）：中间方块顶上的一团火 + 一个暖色点光源。
        // 仅交互/拍摄场景模式下有效（纯天空截图时整组跳过）。
        if (show_scene_ && show_fire_) {
            AppendFire(cmds);
        }

        // 光照面板（仅交互窗口；headless 拍摄是纯 3D 截图）。
        if (show_panel_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 叠加火焰（实验）：中间方块顶上的一团火 + 一个同位置的暖色点光源。
    // 火是“程序化 shader 火焰”（1 个 billboard quad，见 FireRenderer）；
    // 点光源让周围方块被火光染暖（JPOV 版的“rim light”错觉）。
    void AppendFire(jpov::RenderCommandList* cmds) const {
        // 基础高度：中间方块（BoxCenter(1)）顶面 y = 2*kBoxHalf。
        const float base_y = 2.0f * kBoxHalf;
        const jpov::Vec3f base{0.0f, base_y, 0.0f};

        // 火焰参数（内眼调过的一组“壁炉/火盆”量级）：占地~1m，高~1.2m。
        cmds->DrawFire(/*base*/ base,
                       /*radius*/ kFireRadius,
                       /*height*/ kFireHeight,
                       /*color_core*/ {1.0f, 0.86f, 0.48f, 1.0f},   // 亮黄芯
                       /*color_outer*/ {1.0f, 0.20f, 0.03f, 1.0f},   // 橙红外焰
                       /*intensity*/ 1.3f,
                       /*speed*/ 1.6f,
                       /*noise_scale*/ 3.2f,
                       /*blend*/ fire_blend_);

        // 暖色点光源：放在火焰中部，照亮周围方块（“火光照亮环境”）。
        // intensity=1.0 ≈ 100W 白炽灯（见 LIGHT_INTENSITY.md）；火取略高于此。
        jpov::PointLight light;
        light.position = {base.x(), base.y() + 0.5f * kFireHeight, base.z()};
        light.color = {1.0f, 0.55f, 0.20f, 1.0f};
        light.linear_radius = 8.0f;
        light.intensity = 4.0f;
        cmds->point_lights.push_back(light);
    }

    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        SkylightApp* app = static_cast<SkylightApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 光照面板：6 个滑条 = 五个自由度（浊度 / 日光季节色温 / 天体方向 / 月色变红 / 夜空偏蓝）。
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
        const int   kRows    = 7;
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };

        // ① 浊度 [0,8]：看霾化（天色发白）+ 日盘/月盘衰减 + 月晕变宽。
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(0),
                        kTurbidityMin, kTurbidityMax, 1);
        // ② 日光季节色温 [−1,+1]：左蓝偏 / 右红偏 / 中间中性（只偏色温，不改亮度）。
        ui_.SliderFloat("季节色温 日光 (左蓝偏..右红偏)", &deg_.daylight_season,
                        row(1), -1.0f, 1.0f, 2);
        // ③ 天体方向（仰角 + 方位角）。仰角正=太阳当空（日光主光），负=月亮当空
        //    （月光主光）；月亮恒在 −sun_dir，故不再单列月亮滑条。
        ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg, row(2),
                        -90.0f, 90.0f, 0);
        ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(3), 0.0f, 360.0f, 0);
        // ④ 月色变红 [0,1]：0=常月，1=血月（只染月盘 + 月光）。
        ui_.SliderFloat("月色变红 (血月)", &deg_.moon_red, row(4), 0.0f, 1.0f, 2);
        // ⑤ 夜空偏蓝 [0,1]：0=出厂夜色，1=梦幻蓝且更亮（夜色两色整体乘子）。
        ui_.SliderFloat("夜空偏蓝 (梦幻夜)", &deg_.night_blue, row(5), 0.0f, 1.0f, 2);
        // ⑥ 火焰开关（实验）：中间方块顶上的程序化火焰 + 暖色点光源。
        ui_.Checkbox("火焰 Fire (实验)", &show_fire_, row(6));
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    bool show_fire_  = true;   // 火焰开关（面板 toggle / headless setter）
    float effect_time_override_ = -1.0f;  // >=0 时覆盖特效时钟（headless 用）
    jpov::ParticleBlend fire_blend_ = jpov::ParticleBlend::kAlpha;  // 火焰混合模式

    // 火焰几何参数（实验默认值；调手感改这里）。
    static constexpr float kFireRadius = 0.55f;  // 水平半宽（米）
    static constexpr float kFireHeight = 1.6f;   // 向上高度（米）

    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

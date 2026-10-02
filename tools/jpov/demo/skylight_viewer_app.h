// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照来源**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 天光 = `CreateDefaultSkyCommand`（其余参数走默认构造；月亮恒取 moon_dir = −sun_dir）；
//   - 光照 = 由该 SkyCommand **推导**（主平行光 + ambient 色/强），不手配。
//
// 交互面板五个自由度（六个滑条）+ 一个环境光开关：
//   ① 浊度 turb [0,8]
//   ② 季节色温 日光 [−1,+1]（左=蓝偏 / 右=红偏；只染太阳能通道）
//   ③ 天体方向：仰角 [−90,+90] + 方位角 [0,360)（两个滑条，属同一个自由度）
//      仰角正 = 太阳在地平线上（白天，太阳平行光）；负 = 太阳沉下、月亮升到反向
//      等高（夜间，月亮平行光）。月亮方向恒取 moon_dir = −sun_dir，不单独给滑条。
//   ④ 月色变红 [0,1]（0=常月，1=血月；只染月盘 + 月光）
//   ⑤ 夜空偏蓝 [0,1]（0=出厂夜色，1=梦幻蓝且更亮；夜色两色整体乘子）
//   ⑥（开关）三色环境光：开 = ambient 按法线仰角在 [天, 天际线, 地] 间插值（AmbientTricolor），
//      关 = 原单色 ambient（AmbientColor）。供肉眼对比两种环境光。
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

    // ── 蓝人（mixamo_male）：同一份蒙皮 mesh + 骨架，每个方块顶面各一个 T-pose 实例 ──
    // person_mesh_ / person_skeleton_id_ 为 0（未装配）时整段跳过 → 不带人场景零回归。
    // person_instances_ 只存 3 个**静态** SkinnedInstanceState（pose_a==pose_b=0 = 恒等 pose
    //   = 骨架 rest = T-pose），每帧拷一份交给 DrawMeshWithSkeleton（一次 instanced draw）。
    uint32_t person_mesh_ = 0;           // 蒙皮 mesh_id（glb 第一个 primitive）
    uint32_t person_skeleton_id_ = 0;    // RegisterSkeleton 返回的骨架 id
    jpov::PBRMaterial person_material_;  // 蒙皮网格的 PBR 材质
    jpov::GltfObject person_gltf_;       // 资源保活（同 models_ 的所有权约定）
    std::vector<jpov::SkinnedInstanceState> person_instances_;

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

    // 是否使用**三色环境光**（[天, 天际线, 地] 垂直梯度）替代单色 ambient。
    // 供肉眼对比：开=由 SkyCommand 推导的三色（AmbientTricolor），关=原来的单色（AmbientColor）。
    void SetTricolorAmbient(bool on) { tricolor_ambient_ = on; }

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

        // 天光 + 光照：全由五个自由度推导——sky 走 CreateDefaultSkyCommand，
        // 主平行光（白天太阳/夜间月亮）与 ambient 全由该 sky 推导（含夜色项）；
        // moon_season 与夜空蓝覆盖默认值。
        const SkyLighting nl = MakeSkyLighting(deg_, tricolor_ambient_);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 远景仰角雾（空气透视）：几何属性走滑条；**雾色由天光推导**
        // （SkyCommand::ElevationFogColor() = 地平线附近天光色），使远景收敛到天边。
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
            // 蓝人：三份 T-pose 实例**一次** instanced draw（person_instances_ 拷贝传入，
            //   DrawMeshWithSkeleton 按值收 vector）。未装配时 person_mesh_==0 → 跳过。
            if (person_mesh_ != 0 && !person_instances_.empty()) {
                cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                           person_material_, person_instances_);
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

    // 光照面板（两列）：左列 = 五个天光自由度 + 三色 ambient 开关；右列 = 远景仰角雾
    // （开关 + 几何滑条 + 雾色色块）。雾色由天光推导，色块可目视核对“天光 → 雾色”的推理链。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kColW    = 0.45f * w;
        const float kColX[2] = {0.025f * w, 0.515f * w};
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

        // ── 左列：天光自由度 ──
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(0, 0),
                        kTurbidityMin, kTurbidityMax, 1);
        ui_.SliderFloat("季节色温 日光 (左蓝偏..右红偏)", &deg_.daylight_season,
                        row(0, 1), -1.0f, 1.0f, 2);
        ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg,
                        row(0, 2), -90.0f, 90.0f, 0);
        ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(0, 3), 0.0f, 360.0f, 0);
        ui_.SliderFloat("月色变红 (血月)", &deg_.moon_red, row(0, 4), 0.0f, 1.0f, 2);
        ui_.SliderFloat("夜空偏蓝 (梦幻夜)", &deg_.night_blue, row(0, 5), 0.0f, 1.0f, 2);
        ui_.Checkbox("三色环境光 (天/天际线/地)", &tricolor_ambient_, row(0, 6));

        // ── 右列：远景仰角雾 ──
        ui_.Checkbox("远景仰角雾 (空气透视)", &deg_.elev_fog_on, row(1, 0));
        ui_.SliderFloat("起雾距离 m", &deg_.elev_fog_start, row(1, 1), 0.0f, 2000.0f, 2);
        ui_.SliderFloat("满雾距离 m", &deg_.elev_fog_full, row(1, 2), 1.0f, 5000.0f, 1);
        ui_.SliderFloat("仰角带内 °", &deg_.elev_fog_inner_deg, row(1, 3), 0.0f, 20.0f, 1);
        ui_.SliderFloat("仰角带外 °", &deg_.elev_fog_outer_deg, row(1, 4), 0.1f, 40.0f, 1);
        ui_.SliderFloat("雾浓度 σ", &deg_.elev_fog_density, row(1, 5), 0.0f, 8.0f, 2);
        ui_.SliderFloat("雾色增益", &deg_.elev_fog_gain, row(1, 6), 0.0f, 5.0f, 2);
        // 收敛色：true=跟随该方向天空色（推荐，与天无缝）；false=用推导常量色（受“雾色增益”影响）。
        ui_.Checkbox("雾色跟随天空", &deg_.elev_fog_use_sky, row(1, 7));
        // 色块 = 天光推导的地平线天光色 × 增益（“跟随天空”时实际收敛色是逐方向天空色）。
        {
            const jpov::Color fc = MakeSkyLighting(deg_, tricolor_ambient_)
                                       .sky.ElevationFogColor();
            const jpov::Color shown{std::min(fc.r * deg_.elev_fog_gain, 1.0f),
                                    std::min(fc.g * deg_.elev_fog_gain, 1.0f),
                                    std::min(fc.b * deg_.elev_fog_gain, 1.0f), 1.0f};
            ui_.ColorSwatch("雾色推导参考→", shown, row(1, 8));
        }
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    bool tricolor_ambient_ = false;   // 三色环境光开关（默认关=原单色，零回归）
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

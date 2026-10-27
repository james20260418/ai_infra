// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照来源**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 天光 = `CreateDefaultSkyCommand`（其余参数走默认构造；月亮恒取 moon_dir = −sun_dir）；
//   - 光照 = 由该 SkyCommand **推导**（主平行光 + ambient 色/强），不手配。
//
// 交互面板：五个天光自由度（六个滑条）+ 三色环境光开关 + SMAA 开关/阈值 +
// 环境镜面开关/天光消费量：
//   ① 浊度 turb [0,8]
//   ② 季节色温 日光 [−1,+1]（左=蓝偏 / 右=红偏；只染太阳能通道）
//   ③ 天体方向：仰角 [−90,+90] + 方位角 [0,360)（两个滑条，属同一个自由度）
//      仰角正 = 太阳在地平线上（白天，太阳平行光）；负 = 太阳沉下、月亮升到反向
//      等高（夜间，月亮平行光）。月亮方向恒取 moon_dir = −sun_dir，不单独给滑条。
//   ④ 月色变红 [0,1]（0=常月，1=血月；只染月盘 + 月光）
//   ⑤ 夜空偏蓝 [0,1]（0=出厂夜色，1=梦幻蓝且更亮；夜色两色整体乘子）
//   ⑥（开关）三色环境光：开 = ambient 按法线仰角在 [天, 天际线, 地] 间插值（AmbientTricolor），
//      关 = 原单色 ambient（AmbientColor）。供肉眼对比两种环境光。
//   ⑦（开关 + 滑条）环境镜面（IBL specular）：开 = 所有材质从天空 cubemap 取镜面反射，
//      滑条 = 天光消费量 [0,1]（见 interface/render_command.h 的 EnvSpecular）。
//      验收工件 = 第三方块（金属，metallic=1）—— 关时是发黑的块，开时能看见天光/地色被映上去。
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
    // 蓝人的装配参数（装配时由主程序填入）：水面场景要另摆一份"站在桌上"的人，
    // 复用同一份 mesh / 骨架 / 身高归一结果，避免两处各算一套 scale 而分叉。
    float person_scale_ = 1.0f;
    float person_asset_min_y_ = 0.0f;   // 资产局部包围盒 min.y（脚底对齐用）

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

    // ── 场景切换 ──
    //   kBoxes：原有的三方块（低反/高光/金属）+ 地面 —— 验收天光/材质；
    //   kWater：水面打底场景（水下大 box + 桌子 + 桌上的人 + 全局水面）—— 验收水。
    enum class SceneKind { kBoxes = 0, kWater = 1 };
    void SetScene(SceneKind kind) { scene_ = kind; }

    // ── 水面场景资源（Init 后由主程序装配一次）──
    uint32_t sea_box_mesh_ = 0;              // 水下大 box（6×3×6）
    jpov::PBRMaterial sea_box_mat_;          // 深灰石
    jpov::PBRMaterial sea_floor_mat_;        // 海底沙色（复用 ground_mesh_）
    std::vector<ModelSlot> water_models_;    // 水面场景专用模型（桌子）
    // 桌上的人：与三方块场景共用同一份蒙皮 mesh / 骨架，只是另存一组摆放。
    std::vector<jpov::SkinnedInstanceState> water_person_instances_;
    void AddWaterModel(jpov::GltfObject obj, jpov::Vec3f center,
                       jpov::Vec3f up, jpov::Vec3f front, float scale = 1.0f) {
        water_models_.push_back({std::move(obj), center, up, front, scale});
    }

    // 水面开关（headless A/B 用：false = 完全不设 water 命令，验证"关水零回归"）。
    void SetShowWater(bool show) { show_water_ = show; }

    // 水面参数（水位 / 吸收 / 反射…）。level 由面板滑条改，其余走默认值 + 少量滑条。
    jpov::WaterSurface water_ = [] {
        jpov::WaterSurface w;
        w.level = kWaterLevelDefault;
        // 默认**纯水皮**（不消光）：先把界面光学单独看对，体积再另外接（froxel）。
        w.include_volume = false;
        return w;
    }();

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;
    // 相机注视点覆盖（默认空 = ViewConfig::Target() 原点）。仅 headless 拍摄用，
    // 交互面板不暴露（水面场景"水下抬头"需要把目标点抬到水面之上）。
    std::optional<jpov::Vec3f> view_target_;

    // ── 天光自由度（跨帧持有；天光与光照全由它们推导）──
    // 滑条值 → SkyCommand 字段的编码见 skylight_scene.h 的 SkyDegrees。
    SkyDegrees deg_;

    // 是否使用**三色环境光**（[天, 天际线, 地] 垂直梯度）替代单色 ambient。
    // 供肉眼对比：开=由 SkyCommand 推导的三色（AmbientTricolor），关=原来的单色（AmbientColor）。
    void SetTricolorAmbient(bool on) { tricolor_ambient_ = on; }

    // 是否开启 SMAA 后处理抗锯齿（供肉眼 A/B 对比）。headless 拍摄可显式设。
    void SetSmaa(bool on, float threshold = 0.1f) {
        smaa_on_ = on;
        smaa_threshold_ = threshold;
    }

    // 环境镜面（IBL specular）开关 + 天光消费量。headless A/B 拍摄显式设，
    // 交互时走面板（⑦）。默认开、消费量 1.0（即“所有镜面都消费天光”）。
    void SetEnvSpecular(bool on, float sky_scale = 1.0f) {
        env_specular_on_ = on;
        env_specular_scale_ = sky_scale;
    }

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

        // 相机由 view_ 推导（同 model viewer；默认 target=原点）。
        // view_target_ 可覆盖（水面场景"水下抬头看水面"需要把目标抬到水面之上）。
        cmds->camera.position = view_.Position();
        cmds->camera.target   = view_target_.has_value()
                                    ? *view_target_
                                    : jpov_viewer::ViewConfig::Target();
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

        // 环境镜面（IBL specular）：让 metallic 材质从天空 cubemap 取反射。
        // 没这一项时，金属块只有太阳/点光源的直射高光，其余全黑——
        // 三块方块的 index 2（metallic=1, “无漫反射，纯环境反射”）是验收工件。
        if (env_specular_on_) {
            jpov::EnvSpecular es;
            es.sky_scale = env_specular_scale_;
            cmds->env_specular = es;
        }

        // SMAA 后处理抗锯齿（可选）：tone map 之后、2D 之前的最后一站。
        if (smaa_on_) {
            jpov::SmaaConfig sc;
            sc.enabled = true;
            sc.threshold = smaa_threshold_;
            cmds->smaa = sc;
        }

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

        // 场景：三方块（默认）或水面打底场景。
        // show_scene_=false 时整组跳过（headless 拍纯天空用）。
        if (show_scene_ && scene_ == SceneKind::kWater) {
            DrawWaterScene(cmds);
        } else if (show_scene_) {
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

        // 全局水面：只有水面场景才给（三方块场景保持零回归 —— 不设 water 就零开销）。
        if (show_scene_ && scene_ == SceneKind::kWater && show_water_) {
            cmds->DrawWater(water_);
        }

        // 光照面板（仅交互窗口；headless 拍摄是纯 3D 截图）。
        if (show_panel_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 水面打底场景：海底 + 水下大 box + 桌上的人 + （由调用方追加的）全局水面。
    void DrawWaterScene(jpov::RenderCommandList* cmds) {
        // 海底：复用 40×40 地面 mesh，换沙色材质（水面之外的部分保持原来的灰地语义）。
        cmds->DrawObject3D(ground_mesh_, sea_floor_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        // 水下大 box（顶面 y=3）。
        cmds->DrawObject3D(sea_box_mesh_, sea_box_mat_,
                           /*center*/ SeaBoxCenter(),
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        // 桌子（摆在 box 顶面）。
        for (const ModelSlot& m : water_models_) {
            cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
        }
        // 桌上的人（与三方块场景同一份 mesh / 骨架，这里只有 1 个实例）。
        if (person_mesh_ != 0 && !water_person_instances_.empty()) {
            cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                       person_material_, water_person_instances_);
        }
    }

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
        const int   kRows    = 15;
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
        // SMAA 后处理抗锯齿：与右侧“远景仰角雾”并列，单独对比开关 + 阈值（敏感度）。
        ui_.Checkbox("SMAA 抗锯齿", &smaa_on_, row(0, 7));
        ui_.SliderFloat("SMAA 阈值 (小=更敏感)", &smaa_threshold_, row(0, 8),
                        0.02f, 0.5f, 2);
        // 环境镜面（IBL specular）：验收工件是金属块（index 2）。
        // 关 = 只有直射高光（发黑）；开 = 能看见天光渐变与地色被映上去。
        ui_.Checkbox("环境镜面 (天光反射)", &env_specular_on_, row(0, 9));
        ui_.SliderFloat("天光消费量 [0,1]", &env_specular_scale_, row(0, 10),
                        0.0f, 1.0f, 2);

        // ── 左列下半（11~14）：水面场景 ──
        {
            bool water_scene = (scene_ == SceneKind::kWater);
            if (ui_.Checkbox("场景: 水面打底 (关=三方块)", &water_scene, row(0, 11))) {
                scene_ = water_scene ? SceneKind::kWater : SceneKind::kBoxes;
            }
        }
        ui_.SliderFloat("水位 m", &water_.level, row(0, 12),
                        kWaterLevelMin, kWaterLevelMax, 2);
        // 水面微起伏（RMS 斜率，度）：0 = 理想镜面（水下仰望时 Snell 窗是物理硬边）；
        // 6 ≈ 微风（默认）；12 ≈ 有浪。只影响界面透射的过渡宽度，不改几何。
        ui_.SliderFloat("水面斜率 RMS °", &water_.surface_slope_rms_deg,
                        row(0, 13), 0.0f, 20.0f, 1);
        ui_.SliderFloat("天光反射量 [0,1]", &water_.sky_scale, row(0, 14),
                        0.0f, 1.0f, 2);

        // ── 右列下半（11~14）：水体光学 ──
        // σa 逐通道（吸收谱，红光死得最快 ⇒ 越深越蓝）；σs 近中性 1/m；
        // g_sun 决定"朝太阳看"的光晕强弱（水是强前向散射）。
        ui_.SliderFloat("吸收 σa 红 (1/m)", &water_.absorption.x(), row(1, 11),
                        0.0f, 1.2f, 3);
        ui_.SliderFloat("吸收 σa 绿 (1/m)", &water_.absorption.y(), row(1, 12),
                        0.0f, 1.2f, 3);
        ui_.SliderFloat("吸收 σa 蓝 (1/m)", &water_.absorption.z(), row(1, 13),
                        0.0f, 1.2f, 3);
        ui_.Checkbox("含水体积(吸收+散射)", &water_.include_volume, row(1, 14));

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
    SceneKind scene_ = SceneKind::kBoxes;
    bool show_water_ = true;
    bool tricolor_ambient_ = false;   // 三色环境光开关（默认关=原单色，零回归）
    bool env_specular_on_ = true;     // 环境镜面开关（默认开：本 PR 的主交付）
    float env_specular_scale_ = 1.0f; // 天光消费量 [0,1]（1 = 全部消费天光）
    bool smaa_on_ = false;            // SMAA 抗锯齿开关（默认关=零回归；交互面板可开）
    float smaa_threshold_ = 0.1f;     // SMAA luma 边缘阈值（越小越敏感）
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

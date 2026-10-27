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
//
// ── 场景（面板顶部的「场景」下拉，2026-10-27 加）──
//   ① 模型场景：三方块 + 地面 + 桌子/橡树 + 三个 T-pose 蓝人（验收天光/材质/受光）；
//   ② 水面场景：一块 **strip 平铺出来的 10 km² 玻璃 quad**（见 skylight_water_quad.h）。
//      切到水面后**面板只留场景下拉 + 一行操作提示** —— 天光那一堆滑条在这个阶段
//      对水面没有意义，占屏反而看不清效果。
//   两个场景**互斥**：水面场景不画模型几何，模型场景不画水面。

#ifndef JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_
#define JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/skylight_water_props.h"
#include "tools/jpov/demo/skylight_water_quad.h"
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
    // 蓝人的归一结果（装配时由主程序填入）：水面场景要把"同一个人"另摆到桌面上，
    // 复用同一份 mesh/骨架/身高归一，避免两处各算一套 scale 而分叉。
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

    // ── 场景切换（面板下拉；两个场景几何互斥）──
    enum class SceneKind { kModel = 0, kWater = 1 };
    void SetScene(SceneKind kind) { scene_ = kind; }

    // 水面开关（headless A/B 用：false = 不画水皮，水下道具就变成"没水"的对照）；
    void SetShowWater(bool show) { show_water_ = show; }

    // 水面用 strip 平铺版还是新水皮 pass（对照用）。默认 false = 走新 pass。
    void SetWaterUseStrip(bool on) { water_use_strip_ = on; }

    // 波浪参数（面板滑条也写这几个字段；headless 拍摄可直接设）。
    // 相位是**弧度**，调用方自己推进（引擎不取时钟 ⇒ 出图可复现）。
    float water_wave_phase_ = 0.0f;
    float water_wave_slope_deg_ = 10.0f;   // 最大斜率（度）= 波幅
    float water_wave_length_m_ = 0.5f;     // 主波长（米）：水下 1 m 处只看得到 ~1.5 m 内的水面，
                                           // 所以波长要**亚米级**才有"整片涟漪"的观感
    float water_wave_dir_deg_ = 30.0f;     // 传播方向（度）
    // 强制透射下限 [0,1]（实验旋钮，非物理）：>0 时灰区（TIR）也会漏出天空。
    float water_min_transmission_ = 0.0f;
    // 水上视角的三项（默认全开；关掉任一项就是 A/B）。
    bool water_use_sky_refl_ = true;      // 镜面天光
    bool water_use_transmit_ = true;      // 水下透射
    bool water_use_sun_glint_ = true;     // 太阳高光

    // 水的折射率。1.333 = 物理水；**1.0 = 关掉折射**（A/B 用：窗内会退化成一刀切的原样天空，
    // 顺带 n² 增益也变成 1）。只为拍图暴露，不进面板。
    void SetWaterIor(float ior) { water_ior_ = ior; }

    // 相机注视点覆盖（默认空 = ViewConfig::Target() 原点）。
    // 只有「水下抬头看水面」这种机位需要它（目标点得抬到水面之上），交互面板不暴露。
    std::optional<jpov::Vec3f> view_target_;

    // ── 水面场景的透射验证道具（布置常量的单一出处：skylight_water_props.h）──
    // 塔形：水下大 box → 小 box → 桌子 → 桌上的人（布局理由见那个头文件）。
    uint32_t water_big_box_mesh_ = 0;      // 6 × 4 × 6（全在水下）
    uint32_t water_small_box_mesh_ = 0;    // 2.4 × 0.8 × 2.4（全在水下）
    jpov::PBRMaterial water_big_box_mat_;
    jpov::PBRMaterial water_small_box_mat_;
    // 水面场景的桌子：**另一份 GltfObject**（glTF 资源按加载独占，不与模型场景共享）。
    jpov::GltfObject water_table_;
    jpov::Vec3f water_table_center_{0.0f, 0.0f, 0.0f};
    float water_table_scale_ = 1.0f;
    // 桌上的人（与模型场景同一份蒙皮 mesh/骨架，这里只有 1 个实例）。
    std::vector<jpov::SkinnedInstanceState> water_person_instances_;

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

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

        // 相机由 view_ 推导（同 model viewer；target=原点）。
        // 远裁剪面按场景给：水面是一块 10 km² 的大平面（对角 ~4.5 km），
        // 用模型场景的 1000 m 会把水面远端切掉、露出背景天空；模型场景保持原值（零回归）。
        const bool water_scene = (scene_ == SceneKind::kWater);
        cmds->camera.position = view_.Position();
        cmds->camera.target   = view_target_.has_value()
                                    ? *view_target_
                                    : jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = water_scene ? kWaterSceneFarM : 1000.0f;

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

        // ── 水皮（新 pass）──
        // 直连 Renderer（暂不进 RenderCommandList）：有值 ⇒ 本帧在 froxel 之后画一次水面。
        // 参数与 strip 版**同源**（同一个头文件的水体色/菲涅耳），两个实现的观感可直接对照。
        // 只有「水面场景 + 画水 + 不用 strip 版」才开；其余情况显式关。
        if (show_scene_ && water_scene && show_water_ && !water_use_strip_) {
            const jpov::Color body = WaterGlassBodyColor();
            const jpov::StripFresnel fr = WaterGlassFresnel();
            jpov::WaterSkinParams wp;
            wp.level_y       = kWaterPlaneY;
            wp.half_extent_m = 0.5f * WaterQuadSideM();
            wp.body_color    = body;
            wp.body_alpha    = body.a;
            wp.rim_color     = {fr.rim_color.x(), fr.rim_color.y(), fr.rim_color.z(), 1.0f};
            wp.use_sky_reflection = water_use_sky_refl_;
            wp.use_transmission   = water_use_transmit_;
            wp.use_sun_glint      = water_use_sun_glint_;
            wp.sky_scale          = 1.0f;
            // 水下视角：临界角外用地色补齐。必须取**天穹下半球的填色**（辐亮度量级，
            // 近黑的 0.05/0.06/0.08）—— 不能用 AmbientTricolor()[2]（那是环境光量级、
            // 还被均值归一过，填充出来比窗内的天空还亮，画面上是反的）。
            wp.tir_color     = nl.sky.ground_color;
            wp.ior           = water_ior_;
            wp.min_transmission = water_min_transmission_;
            wp.wave_phase    = water_wave_phase_;
            wp.wave_slope_deg = water_wave_slope_deg_;
            wp.wave_length_m = water_wave_length_m_;
            wp.wave_dir_deg  = water_wave_dir_deg_;
            SetWaterSkin(wp);
        } else {
            SetWaterSkin(std::nullopt);
        }

        // 场景：模型场景（灰色地面 + 三方块 + 桌子/橡树 + 蓝人）或水面场景（10 km² 玻璃 quad）。
        // show_scene_=false 时两个场景都跳过（headless 拍纯天空用）。
        if (show_scene_ && water_scene) {
            DrawWaterScene(cmds);
        } else if (show_scene_) {            cmds->DrawObject3D(ground_mesh_, mat_ground_,
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
    // 水面场景：水下大 box + 小 box + 桌子 + 桌上的人，最后是水皮本身（strip 平铺的 10 km² quad）。
    //
    // 为什么水皮**最后**画：水皮是半透明 pass（覆盖集），它要盖在已经画好的不透明几何上；
    // 条带 pass 本来就在不透明之后，这里放在最后只是让代码读起来与物理顺序一致。
    void DrawWaterScene(jpov::RenderCommandList* cmds) {
        // 水下两件 box（都完全在水面以下，验收：隔着水皮能同时看见两件、且褪色一致）。
        cmds->DrawObject3D(water_big_box_mesh_, water_big_box_mat_,
                           /*center*/ WaterBigBoxCenter(),
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawObject3D(water_small_box_mesh_, water_small_box_mat_,
                           /*center*/ WaterSmallBoxCenter(),
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        // 桌子（与小 box 顶面对齐；桌腿跨过水线）。
        if (!water_table_.empty()) {
            cmds->DrawGltfObject(water_table_, water_table_center_,
                                 /*up*/ {0.0f, 1.0f, 0.0f}, /*front*/ {0.0f, 0.0f, 1.0f},
                                 water_table_scale_);
        }
        // 桌上的人（脚踩桌面）。
        if (person_mesh_ != 0 && !water_person_instances_.empty()) {
            cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                       person_material_, water_person_instances_);
        }
        if (show_water_ && water_use_strip_) {
            // 对照实现：整块水皮 = **一次** strip draw call（16 条 Strip32 平铺）。
            // 默认不走——水面由 froxel 之后的水皮 pass 直画（见 OneIteration）。
            cmds->DrawStrip32Batch(MakeWaterQuadBatch());
        }
    }

    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        SkylightApp* app = static_cast<SkylightApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 光照面板：顶部 = 场景下拉（模型 / 水面）；模型场景下面是两列天光滑条，
    // 左列 = 五个天光自由度 + 三色 ambient 开关；右列 = 远景仰角雾
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
        const float kColGap  = kColX[1] - kColX[0] - kColW;
        const float kRowH    = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom  = 16.0f;
        const int   kRows    = 11;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int col, int i) {
            return jpov::UiRect{{kColX[col], top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kColW, kRowH}};
        };

        // ── 场景下拉（两个场景的入口）──
        //   模型场景：贴在滑条网格正上方（网格位置对既有拍摄机位是常量，不动）；
        //   水面场景：网格整组不画，下拉挑到左上角 —— 屏幕上只剩这一个控件，
        //             不再被十余行天光滑条遮住水面。
        //   ⚠️ 下拉的 box 在两场景间不同 ⇒ 切换那一帧会收起展开态（这是我们要的）。
        const bool water_scene = (scene_ == SceneKind::kWater);
        const float kFullWidth = 2.0f * kColW + kColGap;
        const jpov::UiRect combo_box =
            water_scene
                ? jpov::UiRect{{kColX[0], 16.0f}, {kFullWidth, kRowH}}
                : jpov::UiRect{{kColX[0], top - (kRowH + kSpacing)}, {kFullWidth, kRowH}};
        int scene_index = static_cast<int>(scene_);
        if (ui_.Combo("场景", &scene_index, SceneNames(), combo_box)) {
            scene_ = static_cast<SceneKind>(scene_index);
        }

        // 水面场景：**只留水面自己的控件**（场景下拉 + 对照开关 + 四个波浪滑条 + 提示），
        // 天光/雾/环境镜面那些控件一律不画。
        if (water_scene) {
            auto water_row = [&](float i) {
                return jpov::UiRect{{kColX[0], 16.0f + i * (kRowH + kSpacing)},
                                    {kFullWidth, kRowH}};
            };
            ui_.Checkbox("水面用 strip 平铺版（关 = 新水皮 pass）", &water_use_strip_,
                         water_row(1.0f));
            // 波浪只扰法线（几何仍是平面）：相位滑条就是"让水纹动起来"的那根。
            // 水上视角三项（默认全开）：关掉任一项就能看出它贡献了什么。
            ui_.Checkbox("镜面天光（反射探针）", &water_use_sky_refl_, water_row(2.0f));
            ui_.Checkbox("水下透射（关=不透明本体色）", &water_use_transmit_, water_row(3.0f));
            ui_.Checkbox("太阳高光（Blinn-Phong×F）", &water_use_sun_glint_, water_row(4.0f));
            // 界面权重：n 从 1.333 往 1 拉 ⇒ 临界角推向 90°、R 整体下降 ⇒ 灰区逐渐变回透射。
            ui_.SliderFloat("水 n（折射率，1=空气）", &water_ior_, water_row(5.0f),
                            1.0f, 1.6f, 3);
            // 实验旋钮：强制保留一部分透射（TIR 区也不再是"全反射"）。
            ui_.SliderFloat("最小透射 [0,1]（实验）", &water_min_transmission_,
                            water_row(6.0f), 0.0f, 1.0f, 2);
            ui_.SliderFloat("水纹相位 rad", &water_wave_phase_, water_row(7.0f),
                            0.0f, 6.2831853f, 2);
            ui_.SliderFloat("水纹波幅 °(最大斜率)", &water_wave_slope_deg_,
                            water_row(8.0f), 0.0f, 20.0f, 1);
            ui_.SliderFloat("水纹波长 m", &water_wave_length_m_, water_row(9.0f),
                            0.05f, 20.0f, 2);
            ui_.SliderFloat("水纹方向 °", &water_wave_dir_deg_, water_row(10.0f),
                            0.0f, 360.0f, 0);
            ui_.Text("水面 = 10 km² quad  右键拖动=绕行 滚轮=推拉", water_row(11.0f));
            return;
        }

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

    // 面板下拉的选项顺序必须与 SceneKind 的枚举值一致（下标 = 枚举值）。
    static const std::vector<const char*>& SceneNames() {
        static const std::vector<const char*> kNames = {
            "模型场景（方块/桌子/橡树/蓝人）",
            "水面（strip 玻璃 quad 10 km²）",
        };
        return kNames;
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    SceneKind scene_ = SceneKind::kModel;   // 默认模型场景（原行为）
    bool show_water_ = true;                // 水面场景是否画水皮（headless A/B 用）
    bool water_use_strip_ = false;          // 水面用 strip 平铺版（对照）；false = 新水皮 pass
    // 波浪状态见上方的 water_wave_* 公开字段
    float water_ior_ = 1.333f;              // 水的折射率（1.0 = 关掉折射，A/B 用）
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

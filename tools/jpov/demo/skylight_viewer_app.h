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
#include "tools/jpov/effect/particle_fire/fire_particles.h"

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
    uint32_t pillar_mesh_ = 0;           // 立柱（0.6 × 3.0 × 0.6，用于「燃烧的立柱」实验）
    jpov::PBRMaterial mat_low_;          // 低反（rough=1.0, metal=0）
    jpov::PBRMaterial mat_high_;         // 高光（rough=0.05, metal=0）
    jpov::PBRMaterial mat_metal_;        // 金属（metal=1, rough=0.15）
    jpov::PBRMaterial mat_ground_;       // 灰色地面
    jpov::PBRMaterial mat_pillar_;       // 立柱（木色）

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

    // 是否绘制场景几何（三方块 + 地面 + 立柱）。headless 拍“天空本身”时置 false。
    void SetShowScene(bool show) { show_scene_ = show; }
    // 是否绘制三校方块（拍立柱时置 false，留出空间）。
    void SetShowBoxes(bool show) { show_boxes_ = show; }
    // 立柱中心（拍立柱时挪到原点，便于环绕取景）。
    void SetPillarCenter(const jpov::Vec3f& c) { pillar_center_ = c; }

    // 是否绘制实验性火焰（含其上方的暖色点光源）。
    // 交互面板有一个 toggle；headless 拍摄可用本 setter 单独开/关出图对比。
    void SetShowFire(bool show) { show_fire_ = show; }
    bool show_fire() const { return show_fire_; }

    // 粒子风格（0=软团 / 1=火舌 / 2=涡流）；切换时清池避免混形。
    void SetParticleStyle(int style) {
        particle_style_ = std::max(0, std::min(2, style));
        emitter_.Clear();
        emitter_.SetMode(static_cast<jpov::FireParticleEmitter::Mode>(particle_style_));
    }
    int particle_style() const { return particle_style_; }

    // 粒子是否每帧推进（交互=true 看动态；headless 拍摄时 false + WarmUpParticles 冻结）。
    void SetParticlesLive(bool live) { particles_live_ = live; }

    // 把粒子模拟向前“预热” seconds 秒（固定 dt=1/60），用于 headless 拍到稳定状态。
    void WarmUpParticles(float seconds) {
        const float dt = 1.0f / 60.0f;
        const int steps = static_cast<int>(seconds / dt);
        for (int i = 0; i < steps; ++i) {
            StepParticles(dt);
        }
    }
    size_t alive_particles() const { return emitter_.AliveCount(); }

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
            if (show_boxes_) {
                cmds->DrawObject3D(box_mesh_, mat_low_,   BoxCenter(0),
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
                cmds->DrawObject3D(box_mesh_, mat_high_,  BoxCenter(1),
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
                cmds->DrawObject3D(box_mesh_, mat_metal_, BoxCenter(2),
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
                // 额外模型（桌子 / 橡树）：与方块同开关（拍立柱时一起隐藏）。
                for (const ModelSlot& m : models_) {
                    cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
                }
            }
            // 立柱（燃烧实验）：默认立在方块行前方；拍立柱时可挪到原点并隐藏方块。
            cmds->DrawObject3D(pillar_mesh_, mat_pillar_,
                               /*center*/ pillar_center_,
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f});
        }

        // 火焰/燃烧（粒子实验）：
        //   ① 立柱**粒子燃烧**（粒子化火舌，本阶段主角）
        //   ② 立柱底部环绕火苗（旧“包裹”验证，默认关）
        //   ③ 立柱几何燃烧体（旧，默认关）
        //   ④ 方块顶上一团火（旧“火焰”，默认关）
        if (show_fire_) {
            if (particles_live_) {
                StepParticles(1.0f / static_cast<float>(kViewerFps));
            }
            AppendPillarBurningParticles(cmds);
            if (show_ring_fire_) {
                AppendPillarFire(cmds);
            }
            if (show_burning_body_) {
                AppendPillarBurning(cmds);
            }
            if (show_box_fire_) {
                AppendFire(cmds);
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
    // 立柱底部一圈采集点（粒子发射源）：柱底圆周，略出于柱面。
    void PillarBaseRing(std::vector<jpov::Vec3f>* out) const {
        out->clear();
        constexpr int kRing = 10;
        const float ring = kPillarHalfX + 0.03f;
        const float px = pillar_center_.x();
        const float pz = pillar_center_.z();
        for (int i = 0; i < kRing; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / kRing;
            out->push_back({px + ring * std::cos(a), 0.06f, pz + ring * std::sin(a)});
        }
    }

    // 推进一步粒子模拟（发射 + 各风格动力学 + 老化）。
    void StepParticles(float dt) {
        if (!style_synced_) {           // 首次：把发射器风格同步到 particle_style_
            SetParticleStyle(particle_style_);
            style_synced_ = true;
        }
        std::vector<jpov::Vec3f> src;
        PillarBaseRing(&src);
        // 各风格发射率/扩散不同。
        const float rate = (particle_style_ == 1) ? 520.0f
                         : (particle_style_ == 2) ? 330.0f : 260.0f;
        const float spread = (particle_style_ == 1) ? 0.05f
                           : (particle_style_ == 2) ? 0.09f : 0.10f;
        emitter_.SetRate(rate);
        emitter_.EmitAccumulated(dt, src, spread);
        emitter_.Update(dt, particle_clock_);
        particle_clock_ += dt;
    }

    // 立柱「燃烧」——**粒子化**（三种风格可选）。
    void AppendPillarBurningParticles(jpov::RenderCommandList* cmds) const {
        emitter_.AppendParticles(cmds,
                                 /*color_hot*/  {1.00f, 0.88f, 0.50f, 1.0f},
                                 /*color_cold*/ {0.85f, 0.14f, 0.02f, 1.0f},
                                 /*intensity*/ 1.15f,
                                 /*blend*/ particle_blend_);
        // 暖色点光源：柱底，照亮柱子/地面。
        jpov::PointLight light;
        light.position = {pillar_center_.x(), 0.55f, pillar_center_.z()};
        light.color = {1.0f, 0.50f, 0.16f, 1.0f};
        light.linear_radius = 7.0f;
        light.intensity = 7.0f;
        cmds->point_lights.push_back(light);
    }

    // 立柱「燃烧」：把立柱声明为一个**燃烧体**（几何驱动，BurningRenderer 采样）。
    void AppendPillarBurning(jpov::RenderCommandList* cmds) const {
        cmds->DrawBurning(/*center*/ pillar_center_,
                          /*up*/     {0.0f, 1.0f, 0.0f},
                          /*front*/  {0.0f, 0.0f, 1.0f},
                          /*half_extents*/ {kPillarHalfX, kPillarHalfY, kPillarHalfZ},
                          /*strength*/ 0.8f,
                          /*seed*/ 7u);
        // 暖色点光源：柱底，照亮柱子/地面。
        jpov::PointLight light;
        light.position = {pillar_center_.x(), 0.6f, pillar_center_.z()};
        light.color = {1.0f, 0.50f, 0.16f, 1.0f};
        light.linear_radius = 7.0f;
        light.intensity = 7.0f;
        cmds->point_lights.push_back(light);
    }

    // 叠加火焰（实验）：立柱底部**环状小火焰**——验证“包裹”。
    //
    // 为什么这么做：单个 quad 如果穿过柱轴，柱子会把quad中间吃掉、只剩两侧
    // （一眼假）。改为在柱底**圆周上撒 N 个小火苗**：相机绕柱时，
    // 近侧火苗在柱前、远侧火苗被柱体遮掉（深度测试自动处理）→ “火从柱周烧起”。
    void AppendPillarFire(jpov::RenderCommandList* cmds) const {
        constexpr int   kFlames = 10;
        constexpr float kTwoPi  = 6.28318530717958647692f;
        // 立柱轴心（x/z）取 pillar_center_，允许拍立柱时把柱子挪到原点。
        const float px = pillar_center_.x();
        const float pz = pillar_center_.z();

        for (int i = 0; i < kFlames; ++i) {
            const float a = kTwoPi * static_cast<float>(i) / kFlames + 0.30f;
            // 内外交替贴柱面（内环贴柱、外环略出）→ 有一点前后层次。
            const float ring = kPillarHalfX + ((i % 2 == 0) ? 0.02f : 0.12f);
            const jpov::Vec3f base{px + ring * std::cos(a),
                                   0.05f,
                                   pz + ring * std::sin(a)};
            cmds->DrawFire(/*base*/ base,
                           /*radius*/ 0.16f,
                           /*height*/ 0.45f + 0.07f * static_cast<float>(i % 4),
                           /*color_core*/ {1.0f, 0.86f, 0.48f, 1.0f},
                           /*color_outer*/ {1.0f, 0.20f, 0.03f, 1.0f},
                           /*intensity*/ 1.2f,
                           /*speed*/ 1.2f + 0.25f * static_cast<float>(i % 3),
                           /*noise_scale*/ 3.2f,
                           /*blend*/ fire_blend_);
        }

        // 暖色点光源：放在柱底，照亮柱子/地面。
        jpov::PointLight light;
        light.position = {px, 0.5f, pz};
        light.color = {1.0f, 0.50f, 0.16f, 1.0f};
        light.linear_radius = 6.0f;
        light.intensity = 6.0f;
        cmds->point_lights.push_back(light);
    }

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
    // 滑条贴屏幕下方；实验性火焰控件（火焰开关 + 粒子风格下拉）贴屏幕上方
    //（见 top_row 说明：下拉朝下展开，靠底会被裁掉）。
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
        const float kTop     = 16.0f;
        const int   kRows    = 6;   // 六个天光滑条
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };
        // 实验性火焰控件贴**屏幕上沿**：Combo 的下拉列表朝框下方展开
        //（见 Ui::Combo：list_top = box 底边），贴着屏幕下沿时下拉会被裁掉
        // 看不见。故这两项单独一行组，放最上方。
        auto top_row = [&](int i) {
            return jpov::UiRect{{left, kTop + static_cast<float>(i) * (kRowH + kSpacing)},
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
        // ⑥ 火焰开关（实验）：立柱粒子燃烧。放屏幕上方（见 top_row 说明）。
        ui_.Checkbox("火焰 Fire (实验)", &show_fire_, top_row(0));
        // ⑦ 粒子风格（实验）：软团 / 火舌 / 涡流 —— 切换时清池重发。
        const int before_style = particle_style_;
        ui_.Combo("粒子风格 (软团/火舌/涡流)", &particle_style_,
                  {"软团 puff", "火舌 tongue", "涡流 vortex"}, top_row(1));
        if (particle_style_ != before_style) {
            SetParticleStyle(particle_style_);
        }
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    bool show_fire_  = true;   // 火焰/燃烧总开关
    bool show_box_fire_ = false;  // 旧的“方块顶上一团火”对比项（默认关）
    bool show_ring_fire_ = false; // 旧的“柱底环状火焰”对比项（默认关）
    bool show_burning_body_ = false;  // 旧的“几何燃烧体”对比项（默认关）
    bool particles_live_ = true;      // 粒子是否每帧推进（交互=true）

    // 粒子发射器（有状态的模拟层；固定池，不无限增长）。
    jpov::FireParticleEmitter emitter_{1500};
    int particle_style_ = 1;          // 0=软团 / 1=火舌 / 2=涡流（默认火舌）
    bool style_synced_ = false;       // 首次 StepParticles 时同步发射器风格
    float particle_clock_ = 0.0f;     // 涡流场的演化时钟
    jpov::ParticleBlend particle_blend_ = jpov::ParticleBlend::kAdditive;
    bool show_boxes_ = true;      // 三校方块开关（拍立柱时关）
    // 立柱中心（拍立柱时可挪到原点）。
    jpov::Vec3f pillar_center_{kPillarCx, kPillarHalfY, kPillarCz};
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

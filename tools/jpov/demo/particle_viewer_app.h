// JPOV 粒子查看器（particle viewer）— 渲染核心 App
//
// 目的：展示本次试验最终选定的 7 种火焰 puff，并把控制项收敛为一组统一滑条：
//   粒子数 / 时间整体快慢 / 燃料块（正方体；开关 + X/Y/Z 平移 + scale）/
//   烟雾（开关+整体照度）/ 火焰温度增益（含 3 套温度→颜色映射）
//
// 设计：
//   * 纹理：5 张 12×6「成对渐变」atlas（kPairMorph，pair_len=12，一块 = 两段形变）
//           + ben7（10×6 循环）+ hwrow（25×1 循环）
//   * 颜色：粒子本身不带温度通道；这里用归一化年龄推导温度 tau，
//           再经 fire_palette.h 的 3 套映射得到颜色（Blackbody / Cinematic / Cute）。

#ifndef JPOV_DEMO_PARTICLE_VIEWER_APP_H_
#define JPOV_DEMO_PARTICLE_VIEWER_APP_H_

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/fire_palette.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/particle_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/particles/particle_system.h"

namespace jpov_particle {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 7 种正式火焰纹理的采样方式。
enum class FirePlay : uint8_t {
    kLoop = 0,   // 整张 atlas 按 fps 循环（ben7 / hwrow）
    kPair = 1,   // 每粒子随机选一块，寿命走完块首→块尾（12×6 形变 atlas）
};

struct FireTexEntry {
    const char* label;
    const char* file;
    int tx;
    int ty;
    FirePlay play;
    int pair_len;   // kPair 用
    float fps;      // kLoop 用
};

inline const FireTexEntry kFireTexes[] = {
    {"V4  手绘+M5",        "mp/flames_pairs_v4_12x6.png",   12, 6, FirePlay::kPair, 12, 0.0f},
    {"V3.4 F5_v3/S平滑",   "mp/flames_pairs_v34_12x6.png",  12, 6, FirePlay::kPair, 12, 0.0f},
    {"V7.1 F5=手绘",       "mp/flames_pairs_v71_12x6.png",  12, 6, FirePlay::kPair, 12, 0.0f},
    {"V6.1 S5=手绘",       "mp/flames_pairs_v61_12x6.png",  12, 6, FirePlay::kPair, 12, 0.0f},
    {"V8  可爱",           "mp/flames_pairs_v8_12x6.png",   12, 6, FirePlay::kPair, 12, 0.0f},
    {"ben7",               "candidates/cand07_ben7.png",    10, 6, FirePlay::kLoop,  2, 24.0f},
    {"hwrow",              "candidates/cand10_hwrow.png",   25, 1, FirePlay::kLoop,  2, 12.0f},
};
inline constexpr int kFireTexCount =
    static_cast<int>(sizeof(kFireTexes) / sizeof(kFireTexes[0]));

inline jpov::particles::EmitterConfig MakeFireConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.22f, 0.0f};
    c.rate = 220.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.10f;
    c.cone_half_angle_deg = 32.0f;
    c.lifetime = {0.75f, 1.10f};
    c.speed = {0.20f, 0.45f};
    c.size_start = {0.15f, 0.21f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 0.55f, 0.0f};
    c.drag = 1.0f;
    c.noise_enabled = true;
    c.noise_amp = 0.9f;
    c.noise_freq = 1.8f;
    c.use_color_over_life = false;     // 颜色由温度映射接管
    c.use_size_over_life = false;      // 形变由 atlas 负责，尺寸恒定
    c.flipbook = true;
    c.tiles_x = 12;
    c.tiles_y = 6;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kPairMorph;
    c.pair_len = 12;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAdditive;
    return c;
}

inline jpov::particles::EmitterConfig MakeSmokeConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.55f, 0.0f};
    c.rate = 0.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.12f;
    c.cone_half_angle_deg = 20.0f;
    c.lifetime = {1.8f, 2.6f};
    c.speed = {0.35f, 0.65f};
    c.size_start = {0.55f, 0.85f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 0.45f, 0.0f};
    c.drag = 0.8f;
    c.noise_enabled = true;
    c.noise_amp = 0.8f;
    c.noise_freq = 1.1f;
    c.use_color_over_life = true;
    jpov::particles::Gradient g;
    g.keys = {
        {0.00f, jpov::Color{0.05f, 0.05f, 0.06f, 0.00f}},
        {0.20f, jpov::Color{0.05f, 0.05f, 0.06f, 0.75f}},
        {0.70f, jpov::Color{0.07f, 0.07f, 0.08f, 0.55f}},
        {1.00f, jpov::Color{0.09f, 0.09f, 0.10f, 0.00f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.55f}, {1.0f, 1.7f}};
    c.flipbook = false;
    c.tiles_x = 1;
    c.tiles_y = 1;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAlpha;
    c.sort = jpov::SortMode::kByDepth;
    return c;
}

class ParticleViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    uint32_t fuel_mesh_ = 0;
    jpov::PBRMaterial fuel_mat_;

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;

    bool fire_enable_ = true;
    bool smoke_enable_ = true;
    jpov::particles::EmitterConfig fire_cfg_ = MakeFireConfig();
    jpov::particles::EmitterConfig smoke_cfg_ = MakeSmokeConfig();
    jpov::particles::ParticleEmitter fire_emitter_;
    jpov::particles::ParticleEmitter smoke_emitter_;
    bool fire_needs_reset_ = true;
    bool smoke_needs_reset_ = true;
    int fire_choice_ = 0;
    std::array<uint32_t, kFireTexCount> fire_choice_tex_{};
    uint32_t smoke_texture_id_ = 0;
    bool frame_blend_ = true;

    // ---- 统一滑条（Danis 指定）----
    float ui_particle_count_ = 80.0f;    // 目标粒子数（1..100）
    float ui_fire_size_ = 0.18f;         // 粒子世界尺寸（size_start 的上界）
    float ui_time_scale_ = 1.0f;         // 时间整体快慢
    bool ui_fuel_enable_ = true;         // 燃料块（正方体，可平移/缩放）
    float ui_fuel_x_ = 0.0f;             // 燃料块平移 X
    float ui_fuel_y_ = 0.0f;             // 燃料块平移 Y（上下）
    float ui_fuel_z_ = 0.0f;             // 燃料块平移 Z
    float ui_fuel_scale_ = 0.25f;        // 燃料块整体 scale（正方体边长）
    float ui_smoke_bright_ = 1.0f;       // 烟雾整体照度
    float ui_temp_gain_ = 1.0f;          // 火焰温度增益
    int ui_palette_ = 1;                 // 0=Blackbody 1=Cinematic 2=Cute

    jpov::particles::Rng sub_rng_{20261015u};

    void InstallTextMeasure() { ui_.SetTextMeasure(&ParticleViewerApp::TextWidth, this); }
    void SetShowPanel(bool show) { show_panel_ = show; }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
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

        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/false);
        cmds->sky = nl.sky;
        cmds->sun = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        if (ui_fuel_enable_) {
            const float s = ui_fuel_scale_;
            cmds->DrawObject3D(fuel_mesh_, fuel_mat_,
                               {ui_fuel_x_, ui_fuel_y_ + 0.5f * s, ui_fuel_z_},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                               /*scale=*/s);
        }

        // ── 懒加载纹理 ──
        const int fc = (fire_choice_ < 0) ? 0
                     : ((fire_choice_ >= kFireTexCount) ? kFireTexCount - 1 : fire_choice_);
        if (kFireTexes[fc].file[0] != '\0' && fire_choice_tex_[fc] == 0) {
            const std::string rel = std::string("textures/") + kFireTexes[fc].file;
            const std::string p = FindTexture(rel.c_str());
            if (!p.empty()) {
                fire_choice_tex_[fc] = RegisterTexture(p);
                LOG(INFO) << "[particle_viewer] fire tex = " << p;
            }
        }
        if (smoke_texture_id_ == 0) {
            const std::string p = FindTexture("textures/puffs/smoke_soft.png");
            if (!p.empty()) { smoke_texture_id_ = RegisterTexture(p); }
        }

        const float life_avg = 0.5f * (fire_cfg_.lifetime.min + fire_cfg_.lifetime.max);
        const float dt = (1.0f / static_cast<float>(kViewerFps)) * ui_time_scale_;

        // ── 火焰 ──
        std::vector<jpov::Vec3f> flame_deaths;
        std::vector<jpov::ParticleInstance> finst;
        if (fire_enable_) {
            fire_cfg_.rate = ui_particle_count_ / std::max(life_avg, 1e-3f);
            fire_cfg_.size_start = {ui_fire_size_ * 0.6f, ui_fire_size_};
            const float base_y = ui_fuel_enable_
                ? (ui_fuel_y_ + ui_fuel_scale_ + 0.01f) : 0.22f;
            fire_cfg_.origin = {ui_fuel_x_, base_y, ui_fuel_z_};
            fire_cfg_.tiles_x = kFireTexes[fc].tx;
            fire_cfg_.tiles_y = kFireTexes[fc].ty;
            fire_cfg_.start_frame_random = true;
            if (kFireTexes[fc].play == FirePlay::kPair) {
                fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kPairMorph;
                fire_cfg_.pair_len = kFireTexes[fc].pair_len;
                // 这些 atlas 是「RGB=白 / A=形状」的**掩膜**：
                // additive 模式按亮度算覆盖（max(rgb)），会把形状抹成整块方片；
                // 所以走 kAlpha（覆盖 = tex.a），形状才对。
                fire_cfg_.blend = jpov::BlendMode::kAlpha;
            } else {
                fire_cfg_.blend = jpov::BlendMode::kAdditive;
                fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
                fire_cfg_.sheet_fps = kFireTexes[fc].fps;
            }
            if (fire_needs_reset_) {
                fire_emitter_.SetConfig(fire_cfg_);
                fire_emitter_.Reset(20261019u);
                fire_needs_reset_ = false;
            }
            fire_emitter_.SetConfig(fire_cfg_);
            fire_emitter_.Update(dt, &finst, &flame_deaths);

            // 温度 → 颜色（统一调色入口）
            for (jpov::ParticleInstance& p : finst) {
                const float age = std::clamp(p.age01, 0.0f, 1.0f);
                const float tau = std::clamp((1.0f - age) * ui_temp_gain_, 0.0f, 1.0f);
                jpov::Color c = FirePaletteColor(ui_palette_, tau);
                const float fade_in = std::min(1.0f, age / 0.05f);
                const float fade_out = 1.0f - std::max(0.0f, (age - 0.72f) / 0.28f);
                c.a = std::clamp(fade_in * fade_out, 0.0f, 1.0f);
                p.color = c;
            }
        }

        // ── 烟 ──
        if (smoke_enable_) {
            if (smoke_needs_reset_) {
                smoke_emitter_.SetConfig(smoke_cfg_);
                smoke_emitter_.Reset(20261015u);
                smoke_needs_reset_ = false;
            }
            const float smoke_y = ui_fuel_enable_
                ? (ui_fuel_y_ + ui_fuel_scale_ + 0.25f) : 0.55f;
            smoke_cfg_.origin = {ui_fuel_x_, smoke_y, ui_fuel_z_};
            smoke_emitter_.SetConfig(smoke_cfg_);
            for (const jpov::Vec3f& dp : flame_deaths) {
                if (sub_rng_.Next01() < 0.55f) { smoke_emitter_.SpawnAt(dp); }
            }
            std::vector<jpov::ParticleInstance> sinst;
            smoke_emitter_.Update(dt, &sinst);
            for (jpov::ParticleInstance& si : sinst) {
                si.color.r *= ui_smoke_bright_;
                si.color.g *= ui_smoke_bright_;
                si.color.b *= ui_smoke_bright_;
            }
            if (!sinst.empty() && smoke_texture_id_ != 0) {
                jpov::ParticleRenderCommand spc;
                spc.particles = std::move(sinst);
                spc.texture_id = smoke_texture_id_;
                spc.tiles_x = 1;
                spc.tiles_y = 1;
                spc.flipbook = false;
                spc.align = smoke_cfg_.align;
                spc.blend = smoke_cfg_.blend;
                spc.sort = smoke_cfg_.sort;
                spc.frame_blend = frame_blend_;
                cmds->DrawParticles(std::move(spc));
            }
        }

        const uint32_t fire_tex = fire_choice_tex_[fc];
        if (fire_enable_ && !finst.empty() && fire_tex != 0) {
            jpov::ParticleRenderCommand pc;
            pc.particles = std::move(finst);
            pc.texture_id = fire_tex;
            pc.tiles_x = fire_cfg_.tiles_x;
            pc.tiles_y = fire_cfg_.tiles_y;
            pc.flipbook = fire_cfg_.flipbook;
            pc.align = fire_cfg_.align;
            pc.blend = fire_cfg_.blend;
            pc.sort = fire_cfg_.sort;
            pc.frame_blend = frame_blend_;
            cmds->DrawParticles(std::move(pc));
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float TextWidth(const char* text, float font_size, const char* /*alias*/,
                           void* userdata) {
        ParticleViewerApp* app = static_cast<ParticleViewerApp*>(userdata);
        return app->MeasureTextWidth(std::string(), text ? text : "", font_size);
    }

    static bool FileExists(const char* path) {
        std::ifstream f(path);
        return f.good();
    }
    static std::string ExeDir() {
#ifndef _WIN32
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            const std::string p(buf);
            const size_t s = p.find_last_of('/');
            if (s != std::string::npos) { return p.substr(0, s); }
        }
#endif
        return std::string();
    }
    static std::string FindTexture(const char* rel) {
        const std::string d = ExeDir();
        if (!d.empty()) {
            const std::string p = d + "/" + rel;
            if (FileExists(p.c_str())) { return p; }
        }
        if (FileExists(rel)) { return std::string(rel); }
        return std::string();
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(15.0f);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float x0 = 0.02f * w;
        {
            std::vector<const char*> items;
            items.reserve(static_cast<size_t>(kFireTexCount));
            for (int i = 0; i < kFireTexCount; ++i) { items.push_back(kFireTexes[i].label); }
            ui_.Combo("火焰纹理", &fire_choice_, items,
                      jpov::UiRect{{x0, 8.0f}, {0.30f * w, 26.0f}});
        }
        {
            std::vector<const char*> items;
            for (int i = 0; i < kFirePaletteCount; ++i) {
                items.push_back(kFirePaletteNames[i]);
            }
            ui_.Combo("温度映射", &ui_palette_, items,
                      jpov::UiRect{{x0, 42.0f}, {0.30f * w, 26.0f}});
        }

        const float colw = 0.26f * w;
        const float top = h - 16.0f - (11 * 24.0f + 10 * 5.0f);
        auto row = [&](int i) {
            return jpov::UiRect{{x0, top + i * 29.0f}, {colw, 24.0f}};
        };
        ui_.Checkbox("粒子火", &fire_enable_, row(0));
        ui_.SliderFloat("粒子数", &ui_particle_count_, row(1), 1.0f, 100.0f, 0);
        ui_.SliderFloat("时间快慢", &ui_time_scale_, row(2), 0.1f, 2.5f, 2);
        ui_.Checkbox("燃料块", &ui_fuel_enable_, row(3));
        ui_.SliderFloat("燃料块 X", &ui_fuel_x_, row(4), -0.8f, 0.8f, 2);
        ui_.SliderFloat("燃料块 Y", &ui_fuel_y_, row(5), -0.3f, 0.8f, 2);
        ui_.SliderFloat("燃料块 Z", &ui_fuel_z_, row(6), -0.8f, 0.8f, 2);
        ui_.SliderFloat("燃料块 scale", &ui_fuel_scale_, row(7), 0.05f, 0.6f, 2);
        ui_.Checkbox("烟雾", &smoke_enable_, row(8));
        ui_.SliderFloat("烟雾照度", &ui_smoke_bright_, row(9), 0.0f, 3.0f, 2);
        ui_.SliderFloat("温度增益", &ui_temp_gain_, row(10), 0.5f, 2.0f, 2);
    }

    bool show_panel_ = true;
    jpov::Ui ui_;
};

}  // namespace jpov_particle

#endif  // JPOV_DEMO_PARTICLE_VIEWER_APP_H_

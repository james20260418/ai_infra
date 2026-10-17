// JPOV 粒子查看器（particle viewer）— 渲染核心 App
//
// 目的：验证「广告牌粒子 → WBO 半透明分级纹理（1/2）→ 合成 → froxel」这条链的最小测试场。
// 场景 = 灰色地面 + 一张桌子 + 桌上的「火 + 烟」（火焰走新 ParticleRenderer 的 WBO 路径）。
// 与 fog viewer 同构（同一套 JPOV::OneIteration 渲染体 + 即时模式 UI）。
//
// 设计：tools/jpov/docs/jpov_fragment_overdraw_design.md（低分半透明粒子 + WBO）。

#ifndef JPOV_DEMO_PARTICLE_VIEWER_APP_H_
#define JPOV_DEMO_PARTICLE_VIEWER_APP_H_

#include <array>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "tools/jpov/include/jpov/jpov.h"
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

// ff viewer 可选火纹理（下拉切换）：0 = 程序化 puff；其余 = CC0 火纹理 flipbook。
struct FireTexEntry {
    const char* label;
    const char* file;   // 相对 textures/ 的路径；空 = 程序化 puff
    int tx;
    int ty;
};
inline const FireTexEntry kFireTexes[] = {
    {"火#1  ben1",        "candidates/cand01_ben1.png",    10, 6},
    {"火#2  ben2",        "candidates/cand02_ben2.png",    10, 6},
    {"火#3  ben3",        "candidates/cand03_ben3.png",    10, 6},
    {"火#4  ben4",        "candidates/cand04_ben4.png",    10, 6},
    {"火#5  ben5",        "candidates/cand05_ben5.png",    10, 6},
    {"火#6  ben6",        "candidates/cand06_ben6.png",    10, 6},
    {"火#7  ben7",        "candidates/cand07_ben7.png",    10, 6},
    {"火#8  ben8",        "candidates/cand08_ben8.png",    10, 6},
    {"火#9  hw5x5",       "candidates/cand09_hw5x5.png",   5,  5},
    {"火#10 hwrow",       "candidates/cand10_hwrow.png",   25, 1},
};
inline constexpr int kFireTexCount = static_cast<int>(sizeof(kFireTexes) / sizeof(kFireTexes[0]));

inline jpov::particles::EmitterConfig MakeFireConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.80f, 0.0f};
    c.rate = 160.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.22f;
    c.cone_half_angle_deg = 26.0f;
    c.lifetime = {0.75f, 1.1f};
    c.speed = {0.7f, 1.3f};
    c.size_start = {0.28f, 0.5f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 1.9f, 0.0f};
    c.drag = 0.9f;
    c.noise_enabled = true;
    c.noise_amp = 1.5f;
    c.noise_freq = 1.5f;
    c.use_color_over_life = true;
    c.color_over_life = jpov::particles::MakeDefaultFireGradient();
    c.use_size_over_life = true;
    c.size_curve = jpov::particles::MakeDefaultFireSizeCurve();
    c.flipbook = true;
    c.tiles_x = 10;
    c.tiles_y = 6;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
    c.sheet_fps = 24.0f;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAdditive;
    return c;
}

// 黑烟层（子发射器：火焰死亡处生成）。
inline jpov::particles::EmitterConfig MakeSmokeConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.95f, 0.0f};
    c.rate = 0.0f;
    c.burst = 0;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.14f;
    c.cone_half_angle_deg = 20.0f;
    c.lifetime = {1.8f, 2.6f};
    c.speed = {0.5f, 0.9f};
    c.size_start = {0.85f, 1.35f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 1.0f, 0.0f};
    c.drag = 0.7f;
    c.noise_enabled = true;
    c.noise_amp = 1.0f;
    c.noise_freq = 1.2f;
    c.use_color_over_life = true;
    jpov::particles::Gradient g;
    g.keys = {
        // albedo（不是最终色）：颜色由光照决定 —— 天光 + 太阳直射 + 前向散射。
        // 早前这里是 (0.02,0.02,0.03)（近乎黑），因为那时烟完全不接收光照。
        // 低 albedo（烟灰 ≈0.1）：亮度交给光照，避免「白乎乎的雾」；
        // 峰值 alpha 也压到 0.35 —— WBO 只塌成一个等效层，alpha 过饱和会把
        // 密度结构（cov=1−Π(1−α)）抹平成一块均匀的板。
        {0.00f, jpov::Color{0.14f, 0.14f, 0.15f, 0.00f}},
        {0.22f, jpov::Color{0.15f, 0.15f, 0.16f, 0.65f}},
        {0.70f, jpov::Color{0.16f, 0.16f, 0.17f, 0.50f}},
        {1.00f, jpov::Color{0.18f, 0.18f, 0.19f, 0.00f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.5f}, {1.0f, 1.8f}};
    c.flipbook = true;
    c.tiles_x = 8;
    c.tiles_y = 8;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
    c.sheet_fps = 12.0f;
    c.start_frame_random = true;
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
    uint32_t table_top_mesh_ = 0, table_leg_mesh_ = 0;
    jpov::PBRMaterial table_mat_;

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
    int fire_choice_ = 9;   // 默认 火#10 hwrow
    std::array<uint32_t, kFireTexCount> fire_choice_tex_{};
    uint32_t smoke_texture_id_ = 0;
    bool frame_blend_ = true;
    float ui_fire_rate_ = 160.0f;
    float ui_fire_size_ = 0.5f;
    float ui_fire_tex_fps_ = 24.0f;
    float ui_smoke_prob_ = 0.7f;
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
        DrawTable(cmds);

        // ── 懒加载纹理 ──
        if (fire_choice_ >= 0 && fire_choice_ < kFireTexCount &&
            kFireTexes[fire_choice_].file[0] != '\0' &&
            fire_choice_tex_[fire_choice_] == 0) {
            const std::string rel = std::string("textures/") + kFireTexes[fire_choice_].file;
            const std::string p = FindTexture(rel.c_str());
            if (!p.empty()) {
                fire_choice_tex_[fire_choice_] = RegisterTexture(p);
                LOG(INFO) << "[particle_viewer] fire tex = " << p;
            }
        }
        if (smoke_texture_id_ == 0) {
            const std::string p = FindTexture("textures/smoke_flipbook.png");
            if (!p.empty()) { smoke_texture_id_ = RegisterTexture(p); }
        }

        const float dt = 1.0f / static_cast<float>(kViewerFps);

        // ── 火焰（先推进，拿死亡位置给烟的子发射器）──
        std::vector<jpov::Vec3f> flame_deaths;
        std::vector<jpov::ParticleInstance> finst;
        const int fc = (fire_choice_ < 0) ? 0
                     : ((fire_choice_ >= kFireTexCount) ? kFireTexCount - 1 : fire_choice_);
        if (fire_enable_) {
            fire_cfg_.rate = ui_fire_rate_;
            fire_cfg_.size_start = {ui_fire_size_ * 0.6f, ui_fire_size_};
            fire_cfg_.tiles_x = kFireTexes[fc].tx;
            fire_cfg_.tiles_y = kFireTexes[fc].ty;
            fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
            fire_cfg_.sheet_fps = ui_fire_tex_fps_;
            fire_cfg_.start_frame_random = true;
            if (fire_needs_reset_) {
                fire_emitter_.SetConfig(fire_cfg_);
                fire_emitter_.Reset(20261014u);
                fire_needs_reset_ = false;
            }
            fire_emitter_.SetConfig(fire_cfg_);
            fire_emitter_.Update(dt, &finst, &flame_deaths);
        }

        // ── 黑烟（子发射器）──
        if (smoke_enable_) {
            if (smoke_needs_reset_) {
                smoke_emitter_.SetConfig(smoke_cfg_);
                smoke_emitter_.Reset(20261015u);
                smoke_needs_reset_ = false;
            }
            smoke_emitter_.SetConfig(smoke_cfg_);
            for (const jpov::Vec3f& dp : flame_deaths) {
                if (sub_rng_.Next01() < ui_smoke_prob_) {
                    smoke_emitter_.SpawnAt(dp);
                }
            }
            std::vector<jpov::ParticleInstance> sinst;
            smoke_emitter_.Update(dt, &sinst);
            if (!sinst.empty() && smoke_texture_id_ != 0) {
                jpov::ParticleRenderCommand spc;
                spc.particles = std::move(sinst);
                spc.texture_id = smoke_texture_id_;
                spc.tiles_x = smoke_cfg_.tiles_x;
                spc.tiles_y = smoke_cfg_.tiles_y;
                spc.flipbook = smoke_cfg_.flipbook;
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

    void DrawTable(jpov::RenderCommandList* cmds) {
        const jpov::Vec3f up = {0.0f, 1.0f, 0.0f};
        const jpov::Vec3f front = {0.0f, 0.0f, 1.0f};
        cmds->DrawObject3D(table_top_mesh_, table_mat_, {0.0f, 0.74f, 0.0f}, up, front);
        const float lx = 0.70f, lz = 0.36f;
        const float leg[4][2] = {{-lx, -lz}, {lx, -lz}, {-lx, lz}, {lx, lz}};
        for (int i = 0; i < 4; ++i) {
            cmds->DrawObject3D(table_leg_mesh_, table_mat_,
                               {leg[i][0], 0.35f, leg[i][1]}, up, front);
        }
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
        const float colw = 0.26f * w;
        const float cx = 0.02f * w;
        const float top = h - 16.0f - (5 * 24.0f + 4 * 5.0f);
        auto row = [&](int i) {
            return jpov::UiRect{{cx, top + i * 29.0f}, {colw, 24.0f}};
        };
        ui_.Checkbox("粒子火", &fire_enable_, row(0));
        ui_.SliderFloat("发射率 /s", &ui_fire_rate_, row(1), 0.0f, 300.0f, 0);
        ui_.SliderFloat("尺寸 m", &ui_fire_size_, row(2), 0.1f, 3.0f, 2);
        ui_.SliderFloat("火纹理帧率 fps", &ui_fire_tex_fps_, row(3), 6.0f, 60.0f, 0);
        ui_.Checkbox("黑烟", &smoke_enable_, row(4));
    }

    bool show_panel_ = true;
    jpov::Ui ui_;
};

}  // namespace jpov_particle

#endif  // JPOV_DEMO_PARTICLE_VIEWER_APP_H_

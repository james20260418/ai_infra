// JPOV 粒子查看器（particle viewer）— 渲染核心 App
//
// 目的：验证「广告牌粒子 → WBO 半透明分级纹理（1/2）→ 合成 → froxel」这条链的最小测试场。
// 场景 = 灰色地面 + 一张桌子 + 桌上的「火 + 烟」（火焰走新 ParticleRenderer 的 WBO 路径）。
// 与 fire_fog viewer 同构（同一套 JPOV::OneIteration 渲染体 + 即时模式 UI）。
//
// 设计：tools/jpov/docs/jpov_fragment_overdraw_design.md（低分半透明粒子 + WBO）。

#ifndef JPOV_DEMO_PARTICLE_VIEWER_APP_H_
#define JPOV_DEMO_PARTICLE_VIEWER_APP_H_

#include <array>
#include <cmath>
#include <cstdio>
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

// ff viewer 可选火纹理（下拉切换）：file 为空 = 程序化 puff。
// **play 决定完整粒子模式下怎么采样 sheet** —— 以前只看下标写死，
// 结果任何新加的 atlas 都会被当成「整图一帧」（= 一大张图顶在每颗粒子上）。
enum class FireTexPlay : uint8_t {
    kStill = 0,     // 单帧贴图（1×1）
    kLoop = 1,      // 完整粒子：每个粒子按 fps 循环整张 atlas；单团：按 fps 循环
    kPair = 2,      // 成对渐变：一生只走一对（A→B）
    kLifetime = 3,  // 一生走完整张 atlas 一次（0→末帧），不随机相位
};
struct FireTexEntry {
    const char* label;
    const char* file;   // 相对 textures/ 的路径；空 = 程序化 puff
    int tx;
    int ty;
    FireTexPlay play;
    int pair_len = 2;   // kPair 专用：每对占几帧（2=旧版 A/B 交叉淡化）
};
inline const FireTexEntry kFireTexes[] = {
    // —— Astralax 文章的手绘火舌（灰度掩膜，颜色由 color_over_life 给）——
    {"MP 单舌 F1",  "mp/mask_1.png",            1, 1, FireTexPlay::kStill},
    {"MP 单舌 F3",  "mp/mask_3.png",            1, 1, FireTexPlay::kStill},
    {"MP 单舌 F5",  "mp/mask_5.png",            1, 1, FireTexPlay::kStill},
    {"MP 成对渐变(一生一段)", "mp/flames_pairs_12x1.png", 12, 1, FireTexPlay::kPair},
    {"MP 循环形变(F1F3F5)",  "mp/flames_morph_6x1.png",   6, 1, FireTexPlay::kLoop},
    // —— 贝塞尔曲线生成的「火团」（多舌簇 + fBm 撕洞），32 帧无缝循环 ——
    // 由 genVolume/flame_curve_fit.py 反解 F1/F3/F5 → flame_cluster_gen.py 生成。
    {"曲线火团(向量生成)",   "mp/curve_cluster_8x4.png",  8, 4, FireTexPlay::kLoop},
    // —— 控制点插值：起点 F1 → 终点 F5，一生走完整张 sheet 一次 ——
    {"曲线形变 F1→F5",       "mp/curve_morph_f1f5_8x4.png", 8, 4, FireTexPlay::kLifetime},
    // —— FS1：Doubao/Seedance 2.0 内插值视频转成的单团（32 帧，起点→终点单向）——
    {"FS1 (seedance 32f)",   "mp/fs1_32f_8x4.png",        8, 4, FireTexPlay::kLifetime},
    // —— 成对渐变 v2：每颗粒子一条「两段」渐变链（12 帧），中间帧预烘 ——
    // 运行时一个寿命走完 12 帧 = 两段；起点/终点随机（6 条链里随机选）。
    // 纹理可在 FLOW / PROFILE 之间切换（面板 toggle）。
    {"MP 成对渐变 v2(两段)", "mp/flames_pairs_flow_2seg_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v3：以相邻原图的 PROFILE 中点 S1..S5 为基准，再在 S 之间做两段 PROFILE 渐变 ——
    {"MP 成对渐变 v3(两段/PROFILE)", "mp/flames_pairs_v3_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v4：Danis 手绘 5 图 + M5 柔化，再接 V3 的 S1..S5 两段链 ——
    {"MP 成对渐变 v4(手绘+M5)", "mp/flames_pairs_v4_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v5：手绘**原始**笔触（不做 M5）+ V3 的 S1..S5 两段链 ——
    {"MP 成对渐变 v5(手绘原始)", "mp/flames_pairs_v5_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v6：手绘原图 + S5 换成 danis_S5.png 的第 4 个候选（宽扁）——
    {"MP 成对渐变 v6(S5=手绘)", "mp/flames_pairs_v6_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v7：手绘原图 + F5 换成 danis_S5.png 的第 4 个候选 ——
    {"MP 成对渐变 v7(F5=手绘)", "mp/flames_pairs_v7_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v6.1 / v7.1：在 v6/v7 基础上对源图做旋转平均（limited ±15°）——
    {"MP 成对渐变 v6.1(旋转平均)", "mp/flames_pairs_v61_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    {"MP 成对渐变 v7.1(旋转平均)", "mp/flames_pairs_v71_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v3.1：V3 管线，但 5 张源图换成 Danis 手绘简单版（danis_F5_v2.png）——
    {"MP 成对渐变 v3.1(手绘简单版)", "mp/flames_pairs_v31_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v3.1 的 F 源图做旋转平均（削外缘尖角）：±15° / ±30° ——
    {"MP 成对渐变 v3.1r15(削尖)", "mp/flames_pairs_v31r15_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    {"MP 成对渐变 v3.1r30(削尖)", "mp/flames_pairs_v31r30_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v3.3 / v3.4：F5 换成 danis_F5_v3 候选 #2；3.4 对 S 做旋转平均 ——
    {"MP 成对渐变 v3.3(F5_v3)", "mp/flames_pairs_v33_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    {"MP 成对渐变 v3.4(S旋转平均)", "mp/flames_pairs_v34_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    // —— v8：可爱风格（danis_F5_v4.png，灰底抠图）；v8s = S 上做旋转平滑 ——
    {"MP 成对渐变 v8(可爱)", "mp/flames_pairs_v8_12x6.png", 12, 6, FireTexPlay::kPair, 12},
    {"MP 成对渐变 v8s(可爱/S平滑)", "mp/flames_pairs_v8s_12x6.png", 12, 6, FireTexPlay::kPair, 12},
};
inline constexpr int kFireTexCount = static_cast<int>(sizeof(kFireTexes) / sizeof(kFireTexes[0]));
// 「两段成对渐变」纹理在表里的下标（供 PROFILE 切换用）
inline constexpr int kPairV2Index = 8;

// 篝火火焰：一小团一小团的小火苗（BenHickling fire1, CC0, 64px × 60 帧）。
// 单个粒子 = 一根小火苗；篝火 = 几十根叠在一起（“puff 叠层”）。
inline jpov::particles::EmitterConfig MakeFireConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.13f, 0.0f};
    c.rate = 210.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.10f;
    c.cone_half_angle_deg = 54.0f;      // 文章：发射角 36°~144°
    c.lifetime = {0.70f, 1.10f};
    c.speed = {0.25f, 0.55f};
    c.size_start = {0.15f, 0.22f};
    c.rotation = {0.0f, 6.2831853f};    // 出生朝向随机
    c.rotation_speed = {-1.4f, 1.4f};   // 自旋
    c.rotation_speed_decay = 0.85f;     // 自旋随寿命衰减（文章做法）
    c.gravity = {0.0f, 0.55f, 0.0f};    // 正 = 浮力
    c.drag = 1.1f;
    c.noise_enabled = true;
    c.noise_amp = 0.8f;
    c.noise_freq = 1.8f;
    c.use_color_over_life = true;
    // 文章的颜色曲线（贴图是灰度，颜色全靠它）：白黄 → 橙 → 暗红 → 黑
    jpov::particles::Gradient fg;
    // 注意：alpha 现在**就是覆盖**（预乘契约），所以淡入淡出必须写在 alpha 上：
    // 出生 0 → 8% 到 1 → 55% 保持 → 寿命末 0（否则粒子会一直"亮着"直到被删）
    fg.keys = {
        {0.00f, jpov::Color{1.10f, 1.05f, 0.90f, 0.00f}},   // 出生透明
        {0.08f, jpov::Color{1.15f, 1.00f, 0.72f, 1.00f}},   // 淡入
        {0.25f, jpov::Color{1.10f, 0.85f, 0.34f, 0.95f}},
        {0.45f, jpov::Color{0.95f, 0.48f, 0.10f, 0.75f}},   // 开始明显淡出
        {0.70f, jpov::Color{0.55f, 0.16f, 0.02f, 0.32f}},
        {0.88f, jpov::Color{0.20f, 0.03f, 0.00f, 0.00f}},   // 88% 就彻底没了（剩下是"空档"）
        {1.00f, jpov::Color{0.20f, 0.03f, 0.00f, 0.00f}},
    };
    c.color_over_life = fg;
    c.use_size_over_life = true;        // 出生 55% → 峰值 → 末端收缩
    c.size_curve.keys = {{0.0f, 0.55f}, {0.30f, 1.00f}, {0.70f, 0.95f}, {1.0f, 0.45f}};
    c.flipbook = false;                 // 静态纹理（不是 flipbook）
    c.tiles_x = 1;
    c.tiles_y = 1;
    c.start_frame_random = false;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAdditive;
    return c;
}

// 烟：一团团小烟球（SmokeAura, CC0, 256px × 30 帧），从火焰死亡处升起。
inline jpov::particles::EmitterConfig MakeSmokeConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.42f, 0.0f};
    c.rate = 0.0f;
    c.burst = 0;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.10f;
    c.cone_half_angle_deg = 26.0f;
    c.lifetime = {2.4f, 3.4f};
    c.speed = {0.14f, 0.30f};
    c.size_start = {0.08f, 0.14f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 0.30f, 0.0f};
    c.drag = 0.55f;
    c.noise_enabled = true;
    c.noise_amp = 0.5f;
    c.noise_freq = 1.1f;
    c.use_color_over_life = true;
    jpov::particles::Gradient g;
    g.keys = {
        {0.00f, jpov::Color{0.10f, 0.10f, 0.12f, 0.10f}},
        {0.15f, jpov::Color{0.12f, 0.12f, 0.14f, 0.38f}},
        {0.60f, jpov::Color{0.16f, 0.16f, 0.18f, 0.26f}},
        {1.00f, jpov::Color{0.20f, 0.20f, 0.22f, 0.00f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.9f}, {0.5f, 2.0f}, {1.0f, 3.4f}};
    c.flipbook = true;
    c.tiles_x = 6;
    c.tiles_y = 5;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
    c.sheet_fps = 12.0f;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAlpha;
    c.sort = jpov::SortMode::kByDepth;
    return c;
}

// 火星层（验证 kVelocity + stretched 路径：面片平面包含速度 → 拉成条）。
inline jpov::particles::EmitterConfig MakeSparkConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.22f, 0.0f};
    c.rate = 8.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.08f;
    c.cone_half_angle_deg = 30.0f;
    c.lifetime = {0.30f, 0.55f};
    c.speed = {0.5f, 1.1f};
    c.size_start = {0.004f, 0.008f};
    c.rotation = {0.0f, 0.0f};   // 滚转必须 0：stretch 沿「滚转后的局部 Y」= 速度
    c.gravity = {0.0f, 0.7f, 0.0f};
    c.drag = 0.35f;
    c.noise_enabled = true;
    c.noise_amp = 1.2f;
    c.noise_freq = 2.0f;
    c.use_color_over_life = true;
    jpov::particles::Gradient g;
    g.keys = {
        {0.00f, jpov::Color{1.00f, 0.95f, 0.80f, 1.0f}},
        {0.35f, jpov::Color{1.00f, 0.70f, 0.20f, 1.0f}},
        {0.75f, jpov::Color{0.90f, 0.30f, 0.05f, 1.0f}},
        {1.00f, jpov::Color{0.30f, 0.03f, 0.00f, 1.0f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = false;
    c.flipbook = false;          // 纯色小方块（texel = 1）
    c.texture_id = 0;
    c.align = jpov::AlignMode::kVelocity;
    c.stretched = true;
    c.stretch_scale = 4.0f;      // 1 m/s → 高度 ×5
    c.blend = jpov::BlendMode::kAdditive;
    return c;
}

class ParticleViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;

    bool fire_enable_ = true;
    bool smoke_enable_ = true;
    bool sparks_enable_ = false;  // 火星层（kVelocity + stretched）；默认关，面板可开
    bool v2_profile_ = false;     // v2 成对渐变：false=FLOW 预烘，true=PROFILE 预烘
    uint32_t v2_profile_tex_ = 0; // 惰性加载 PROFILE 版 atlas
    bool hide_ground_ = false;    // 拍摄单团时隐藏地面（干净黑底）
    bool single_puff_ = false;    // 观察模式：只画一个 puff，循环看完整张 sheet
    bool single_envelope_ = true;   // 观察模式是否叠加粒子色带（掩膜素材必须叠色带）
    float ui_edge_feather_ = 0.04f; // 边缘羽化（0=关闭）
    float ui_uv_distort_ = 0.02f;   // 形状扰动（0=关闭）——让静态贴图的火舌动起来
    float ui_morph_fps_ = 6.0f;     // 形变模式的形状切换速度（Hz）
    float ui_smoke_bright_ = 1.0f;  // 烟的整体照度
    bool ui_bloom_ = true;          // 辉光开关
    float ui_bloom_intensity_ = 0.8f;
    bool ui_light_ = true;          // 火光点光源开关
    float single_frame_ = 0.0f;   // 观察模式当前帧（仅供面板显示）
    float single_t_ = 0.0f;       // 观察模式寿命进度 [0,1)（仅供面板显示）
    float clock_s_ = 0.0f;        // 内部时间（秒）：RunOnce 的 frame_count 恒为 0，不能依赖它
    jpov::particles::EmitterConfig fire_cfg_ = MakeFireConfig();
    jpov::particles::Gradient fire_color_base_ = fire_cfg_.color_over_life;
    jpov::particles::EmitterConfig smoke_cfg_ = MakeSmokeConfig();
    jpov::particles::EmitterConfig spark_cfg_ = MakeSparkConfig();
    jpov::particles::ParticleEmitter fire_emitter_;
    jpov::particles::ParticleEmitter smoke_emitter_;
    jpov::particles::ParticleEmitter spark_emitter_;
    bool fire_needs_reset_ = true;
    bool smoke_needs_reset_ = true;
    bool sparks_needs_reset_ = true;
    int fire_choice_ = 0;   // 默认 MP 单舌 F1
    std::array<uint32_t, kFireTexCount> fire_choice_tex_{};
    uint32_t smoke_texture_id_ = 0;
    bool frame_blend_ = true;
    float ui_fire_rate_ = 6.0f;
    float ui_fire_size_ = 0.22f;
    float ui_fire_tex_fps_ = 30.0f;
    float ui_smoke_prob_ = 0.35f;
    jpov::particles::Rng sub_rng_{20261015u};

    void InstallTextMeasure() { ui_.SetTextMeasure(&ParticleViewerApp::TextWidth, this); }
    void ResetClock() { clock_s_ = 0.0f; }   // 拍摄对齐用：让单团周期从 t=0 开始
    void SetClock(float s) { clock_s_ = s; }  // 拍摄精确 t 用

    // 「一生走完整张 sheet」的素材（FS1 / 曲线形变）：色带保留颜色，
    // 但 alpha 在整个寿命里保持 1（只在头尾各留 2% 淡入淡出）。
    // 否则默认火色带在 88% 就把 alpha 降到 0 —— 看起来只播了前半段形变。
    jpov::particles::Gradient MorphRamp() const {
        // 「一生走完整张 sheet」专用色带：颜色始终是暖色（白热→橙），
        // 不要末端的暗红——WBO 里暗色 + a=1 会显示成半透明黑板（=消失瞬间发黑）。
        // 消失靠 alpha 在最后 10% 平滑淡出，而不是把颜色压黑。
        jpov::particles::Gradient g;
        struct K { float t; float r, gg, b, a; };
        const K ks[6] = {
            {0.00f, 1.20f, 1.10f, 0.90f, 0.00f},
            {0.03f, 1.20f, 1.00f, 0.72f, 1.00f},
            {0.50f, 1.05f, 0.72f, 0.26f, 1.00f},
            {0.90f, 1.00f, 0.56f, 0.16f, 1.00f},
            {0.97f, 0.98f, 0.50f, 0.13f, 0.45f},
            {1.00f, 0.95f, 0.45f, 0.10f, 0.00f},
        };
        for (const K& k : ks) {
            g.keys.push_back({k.t, jpov::Color{k.r, k.gg, k.b, k.a}});
        }
        return g;
    }    void SetShowPanel(bool show) { show_panel_ = show; }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        clock_s_ += 1.0f / kViewerFps;   // 自己计时（headless RunOnce 的 frame_count 恒 0）
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
        if (ui_bloom_) {
            jpov::BloomConfig bloom;
            bloom.enabled = true;
            bloom.intensity = ui_bloom_intensity_;
            bloom.threshold = 1.0f;
            bloom.levels = 4;
            cmds->bloom = bloom;
        }

        // 火光：一盏暖色点光源（强度按几路正弦抖动 → 篝火跳动感），照亮木柴与地面。
        if (fire_enable_ && ui_light_ && !single_puff_) {
            const float t = clock_s_ * kViewerFps;
            const float flicker = 0.82f + 0.18f *
                (0.5f * std::sin(t * 2.7f) + 0.3f * std::sin(t * 7.3f + 1.2f) +
                 0.2f * std::sin(t * 13.1f + 0.4f));
            jpov::PointLight fire_light;
            fire_light.position = {0.0f, 0.30f, 0.0f};
            fire_light.color = {1.0f, 0.52f, 0.20f, 1.0f};
            fire_light.linear_radius = 4.0f;
            fire_light.intensity = 3.2f * flicker;
            cmds->point_lights.push_back(fire_light);
        }

        if (!hide_ground_) {
            cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, 0.0f, 0.0f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        }

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
        if (FireChoice() == kPairV2Index && v2_profile_tex_ == 0) {
            const std::string p =
                FindTexture("textures/mp/flames_pairs_profile_2seg_12x6.png");
            if (!p.empty()) {
                v2_profile_tex_ = RegisterTexture(p);
                LOG(INFO) << "[particle_viewer] v2 profile tex = " << p;
            }
        }
        if (smoke_texture_id_ == 0) {
            // 烟用「小团 puff」贴图（SmokeAura，CC0）；找不到就退回旧烟纹理。
            const std::string p = FindTexture("textures/puffs/smoke_aura_30f.png");
            const std::string fallback = FindTexture("textures/smoke_flipbook.png");
            if (!p.empty()) {
                smoke_texture_id_ = RegisterTexture(p);
            } else if (!fallback.empty()) {
                smoke_texture_id_ = RegisterTexture(fallback);
            }
        }

        // ── 观察模式：只画一个 puff，把它**从出生到死亡**的完整周期循环播放 ──
        // 用途：调一张 puff 贴图/参数时，不用在一堆粒子里猜它长什么样。
        // 这里的「周期」= 粒子的寿命包络（色带 alpha 淡入淡出 + flipbook 帧推进），
        // 与真实粒子走的是同一套逻辑，只是数量为 1、位置固定。
        if (single_puff_) {
            const int fc = FireChoice();
            const int tiles_x = kFireTexes[fc].tx;
            const int tiles_y = kFireTexes[fc].ty;
            const int total = tiles_x * tiles_y;
            const uint32_t tex =
                (fc == kPairV2Index && v2_profile_) ? v2_profile_tex_
                                                     : fire_choice_tex_[fc];
            if (tex != 0) {
                const float life = 0.5f * (fire_cfg_.lifetime.min + fire_cfg_.lifetime.max);
                // 周期 = 1.25 × 寿命：t∈[0,1] 是粒子的一生，[1,1.25] 是完全消失的空档
                const float cyc = 1.25f;
                const float phase = std::fmod(clock_s_ / life, cyc);
                single_t_ = phase;
                const bool alive = (phase <= 1.0f);
                switch (kFireTexes[fc].play) {
                    case FireTexPlay::kPair: {
                        // 成对渐变：一个周期内只走一对（A→…→B），周期之间换一对
                        const int plen = std::max(2, kFireTexes[fc].pair_len);
                        const int pairs = std::max(1, total / plen);
                        const int cyc = static_cast<int>(clock_s_ / life);
                        const int k = (cyc * 3) % pairs;
                        single_frame_ = static_cast<float>(k * plen) +
                                        std::min(single_t_, 1.0f) *
                                            static_cast<float>(plen - 1);
                        break;
                    }
                    case FireTexPlay::kLifetime:
                        // 一生走完 0 → 末帧（起点 A → 终点 B），不循环
                        single_frame_ = std::min(single_t_, 1.0f) *
                                        static_cast<float>(total - 1);
                        break;
                    case FireTexPlay::kLoop:
                        single_frame_ = std::fmod(
                            clock_s_ * (fc == 4 ? ui_morph_fps_ : ui_fire_tex_fps_),
                            static_cast<float>(total));
                        break;
                    case FireTexPlay::kStill:
                    default:
                        single_frame_ = 0.0f;
                        break;
                }
                jpov::ParticleInstance p;
                p.position = {0.0f, 0.45f, 0.0f};
                p.width = ui_fire_size_ * 0.9f;
                p.height = ui_fire_size_ * 0.9f;
                p.rotation = 0.0f;
                // 默认用**贴图原色**（1,1,1,1）：puff 素材本身是彩色贴图，
                // 再乘一遍色带会二次染色（发灰发粉）。勾上「叠色带」才套粒子色带。
                jpov::Color env_color{1.0f, 1.0f, 1.0f, 1.0f};
                if (single_envelope_) {
                    env_color = (kFireTexes[fc].play == FireTexPlay::kLifetime)
                                    ? MorphRamp().Eval(std::min(single_t_, 1.0f))
                                    : fire_cfg_.color_over_life.Eval(std::min(single_t_, 1.0f));
                }
                p.color = env_color;
                if (!alive) {
                    p.color = jpov::Color{0.0f, 0.0f, 0.0f, 0.0f};
                }
                p.frame = single_frame_;
                jpov::ParticleRenderCommand cmd;
                cmd.particles.push_back(p);
                cmd.texture_id = tex;
                cmd.tiles_x = tiles_x;
                cmd.tiles_y = tiles_y;
                cmd.flipbook = true;
                cmd.frame_blend = true;
                cmd.align = jpov::AlignMode::kView;
                cmd.blend = jpov::BlendMode::kAdditive;
                cmds->DrawParticles(std::move(cmd));
            }
            if (show_panel_) {
                DrawPanel(input);
                ui_.End();
                ui_.Emit(cmds);
            }
            return;
        }

        const float dt = 1.0f / static_cast<float>(kViewerFps);

        // ── 火焰（先推进，拿死亡位置给烟的子发射器）──
        std::vector<jpov::Vec3f> flame_deaths;
        std::vector<jpov::ParticleInstance> finst;
        const int fc = (fire_choice_ < 0) ? 0
                     : ((fire_choice_ >= kFireTexCount) ? kFireTexCount - 1 : fire_choice_);
        // ⚠️ 模式相关的帧采样配置必须在 SetConfig/Update **之前**设置，
        // 否则会被这里覆盖（之前「成对渐变巨快」就是它在每帧把模式改回 kFps@30）。
        // 采样方式**只由 entry 的 play 决定**，不要再按硬编码下标判断。
        const FireTexEntry& fe = kFireTexes[fc];
        if (fire_enable_) {
            fire_cfg_.rate = ui_fire_rate_;
            fire_cfg_.size_start = {ui_fire_size_ * 0.85f, ui_fire_size_};
            fire_cfg_.flipbook = (fe.tx * fe.ty > 1);
            fire_cfg_.tiles_x = fe.tx;
            fire_cfg_.tiles_y = fe.ty;
            switch (fe.play) {
                case FireTexPlay::kPair:
                    fire_cfg_.sheet_time_mode =
                        jpov::particles::SheetTimeMode::kPairMorph;
                    fire_cfg_.pair_len = fe.pair_len;
                    break;
                case FireTexPlay::kLifetime:
                    fire_cfg_.sheet_time_mode =
                        jpov::particles::SheetTimeMode::kLifetime;
                    fire_cfg_.frame_curve.keys = {{0.0f, 0.0f}, {1.0f, 1.0f}};
                    fire_cfg_.sheet_cycles = 1.0f;
                    break;
                case FireTexPlay::kLoop:
                    fire_cfg_.sheet_time_mode =
                        jpov::particles::SheetTimeMode::kFps;
                    fire_cfg_.sheet_fps = (fc == 4) ? ui_morph_fps_ : ui_fire_tex_fps_;
                    break;
                case FireTexPlay::kStill:
                default:
                    fire_cfg_.sheet_time_mode =
                        jpov::particles::SheetTimeMode::kFps;
                    fire_cfg_.sheet_fps = ui_fire_tex_fps_;
                    break;
            }
            fire_cfg_.start_frame_random =
                (fe.play == FireTexPlay::kLoop || fe.play == FireTexPlay::kPair);
            // 「一生走完整张 sheet」的素材：色带 alpha 全程保持 + 尺寸恒定，
            // 保证形变的终点（宽端）真的能被看到。
            if (fe.play == FireTexPlay::kLifetime) {
                fire_cfg_.color_over_life = MorphRamp();
                fire_cfg_.use_size_over_life = false;
            } else {
                fire_cfg_.color_over_life = fire_color_base_;
                fire_cfg_.use_size_over_life = true;
            }
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
            for (jpov::ParticleInstance& si : sinst) {
                si.color.r *= ui_smoke_bright_;
                si.color.g *= ui_smoke_bright_;
                si.color.b *= ui_smoke_bright_;
            }
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
                spc.edge_feather = ui_edge_feather_;
                spc.uv_distort = ui_uv_distort_ * 0.5f;
                spc.time_s = clock_s_;
                cmds->DrawParticles(std::move(spc));
            }
        }
        // 形变模式：atlas 里按 F1→F3→F5→F1… 排列，慢速循环 + 相邻帧混合，
        // 于是每个粒子的火舌会在不同形状之间**渐变**。
        const uint32_t fire_tex =
            (fc == kPairV2Index && v2_profile_) ? v2_profile_tex_
                                                 : fire_choice_tex_[fc];
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
            pc.stretched = fire_cfg_.stretched;
            pc.stretch_scale = fire_cfg_.stretch_scale;
            pc.edge_feather = ui_edge_feather_;
            pc.uv_distort = ui_uv_distort_;
            pc.time_s = clock_s_;
            pc.frame_blend = frame_blend_;
            cmds->DrawParticles(std::move(pc));
        }

        // ── 火星（kVelocity + stretched：验证「面片平面包含速度 → 拉成条」）──
        if (sparks_enable_) {
            if (sparks_needs_reset_) {
                spark_emitter_.SetConfig(spark_cfg_);
                spark_emitter_.Reset(20261018u);
                sparks_needs_reset_ = false;
            }
            spark_emitter_.SetConfig(spark_cfg_);
            std::vector<jpov::ParticleInstance> spark_inst;
            spark_emitter_.Update(dt, &spark_inst);
            if (!spark_inst.empty()) {
                jpov::ParticleRenderCommand spark_cmd;
                spark_cmd.particles = std::move(spark_inst);
                spark_cmd.texture_id = 0;      // 纯色：texel = (1,1,1,1)
                spark_cmd.flipbook = false;
                spark_cmd.align = spark_cfg_.align;
                spark_cmd.blend = spark_cfg_.blend;
                spark_cmd.stretched = spark_cfg_.stretched;
                spark_cmd.stretch_scale = spark_cfg_.stretch_scale;
                cmds->DrawParticles(std::move(spark_cmd));
            }
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
        const float colw = 0.26f * w;
        const float cx = 0.02f * w;
        const float top = h - 16.0f - (15 * 24.0f + 14 * 5.0f);
        auto row = [&](int i) {
            return jpov::UiRect{{cx, top + i * 29.0f}, {colw, 24.0f}};
        };
        ui_.Checkbox("粒子火", &fire_enable_, row(0));
        ui_.SliderFloat("发射率 /s", &ui_fire_rate_, row(1), 0.0f, 300.0f, 0);
        ui_.SliderFloat("尺寸 m", &ui_fire_size_, row(2), 0.1f, 3.0f, 2);
        ui_.SliderFloat("火纹理帧率 fps", &ui_fire_tex_fps_, row(3), 6.0f, 60.0f, 0);
        ui_.Checkbox("黑烟", &smoke_enable_, row(4));
        ui_.Checkbox("火星（速度拉伸）", &sparks_enable_, row(5));
        ui_.Checkbox("单 puff 全周期", &single_puff_, row(6));
        ui_.Checkbox("叠粒子色带", &single_envelope_, row(7));
        ui_.SliderFloat("形状扰动", &ui_uv_distort_, row(8), 0.0f, 0.06f, 3);
        ui_.SliderFloat("边缘羽化", &ui_edge_feather_, row(9), 0.0f, 0.5f, 2);
        ui_.SliderFloat("烟照度", &ui_smoke_bright_, row(10), 0.0f, 3.0f, 2);
        ui_.Checkbox("辉光 bloom", &ui_bloom_, row(11));
        ui_.Checkbox("火光点光源", &ui_light_, row(12));
        if (FireChoice() == kPairV2Index) {
            ui_.Checkbox("V2 用 PROFILE 插值", &v2_profile_, row(13));
        }
        if (single_puff_) {
            const int fc = FireChoice();
            const int total = kFireTexes[fc].tx * kFireTexes[fc].ty;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "t=%.2f  帧 %d/%d  %.0ffps",
                          static_cast<double>(single_t_),
                          static_cast<int>(single_frame_), total,
                          static_cast<double>(ui_fire_tex_fps_));
            ui_.Text(buf, row(13));
        }
    }

    int FireChoice() const {
        if (fire_choice_ < 0) {
            return 0;
        }
        return (fire_choice_ >= kFireTexCount) ? kFireTexCount - 1 : fire_choice_;
    }

    bool show_panel_ = true;
    jpov::Ui ui_;
};

}  // namespace jpov_particle

#endif  // JPOV_DEMO_PARTICLE_VIEWER_APP_H_

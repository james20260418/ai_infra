// JPOV 雾火查看器（fire_fog viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 目的：**肉眼验收 froxel 体积雾**（设计见 tools/jpov/docs/jpov_froxel_design.md）的最小测试场。
// 场景 = 灰色地面 + 一棵高模橡树（oak），点状雾默认摆在橡树处（便于观察雾与实体的深度关系：
// 雾在树前正常叠加，树前的雾被场景深度裁剪，树后的雾被树遮挡）。
//
// 与 skylight viewer 同构（同一套 JPOV::OneIteration 渲染体 + 即时模式 UI 面板约定）：
//   - 场景资源（地面 mesh / 橡树 glTF）由主程序装配后传入；
//   - 光照沿用 skylight_scene.h 的 SkyDegrees + MakeSkyLighting（白昼标准天光，单色 ambient）；
//   - 交互面板暴露**雾体 box** 的可调参数：box 中心 x/y/z、长宽高（size）、外侧圆角壳半径（radius）、
//     消光 σ、衰减剖面（作用于圆角壳）、散射色 albedo、自发光 emission；另加**一盏点光源**（演示 fire_fog 消费点光源）、
//     太阳仰角/方位角/浊度，以及 god ray 的相位/增益。
//
// ⚠️ 雾火**始终走物理光照**（ambient + 太阳×CSM + 点光源）；「无光照」链路已移除。
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + 右键拖拽/滚轮缩放）。

#ifndef JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_
#define JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

#include <array>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>   // readlink（定位 exe 所在目录，与 cwd 解耦）
#endif

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/particle_fire.h"
#include "tools/jpov/demo/skylight_scene.h"   // 复用 SkyDegrees / MakeSkyLighting / 地面材质
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"       // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/particle_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/particles/particle_system.h"

namespace jpov_fire_fog {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// froxel 分辨率可选档（与 FireFogParams.nz / tile_px 对应，面板下拉的候选值）。
// nz 必须是完全平方（见 FireFogParams 约束）。
inline constexpr int kNzChoices[] = {64, 256};
inline constexpr int kTilePxChoices[] = {8, 16, 32};

// 广告牌粒子火的默认配置（一根向上冒的火柱；面板可调关键参数）。
inline jpov::particles::EmitterConfig MakeFireConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.80f, 0.0f};   // 桌面上方
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
    c.tiles_x = 8;
    c.tiles_y = 8;
    // 经典火舌：按**粒子自身寿命**播一遍（点火→峰值→熄灭），非随机相位、不循环。
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kLifetime;
    c.sheet_cycles = 1.0f;
    c.start_frame_random = false;
    c.frame_curve = jpov::particles::MakeDefaultLinearFrameCurve();
    c.align = jpov::AlignMode::kView;   // 朝向相机（Danis 实测：比 vertical 好）
    c.stretched = false;
    c.blend = jpov::BlendMode::kAdditive;
    c.sort = jpov::SortMode::kNone;
    return c;
}

// 黑烟层的默认配置（引擎标准做法：暗色、半透明、大而慢、寿命长、随寿命膨胀上升）。
inline jpov::particles::EmitterConfig MakeSmokeConfig() {
    jpov::particles::EmitterConfig c;
    c.origin = {0.0f, 0.95f, 0.0f};
    c.rate = 0.0f;                    // 黑烟不自己发射：由火焰粒子死亡时子发射器生成
    c.burst = 0;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.14f;
    c.cone_half_angle_deg = 20.0f;
    c.lifetime = {1.8f, 2.6f};
    c.speed = {0.5f, 0.9f};
    c.size_start = {0.85f, 1.35f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 1.0f, 0.0f};   // 上升比火慢
    c.drag = 0.7f;
    c.noise_enabled = true;
    c.noise_amp = 1.0f;
    c.noise_freq = 1.2f;
    c.use_color_over_life = true;
    // 灰黑烟：色偏暗，不透明度先升后降（fade in/out）。
    jpov::particles::Gradient g;
    g.keys = {
        {0.00f, jpov::Color{0.02f, 0.02f, 0.03f, 0.00f}},
        {0.22f, jpov::Color{0.02f, 0.02f, 0.03f, 0.85f}},
        {0.70f, jpov::Color{0.04f, 0.04f, 0.05f, 0.70f}},
        {1.00f, jpov::Color{0.06f, 0.06f, 0.07f, 0.00f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.5f}, {1.0f, 1.8f}};   // 随寿命膨胀
    c.flipbook = true;
    c.tiles_x = 8;
    c.tiles_y = 8;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
    c.sheet_fps = 12.0f;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.stretched = false;
    c.blend = jpov::BlendMode::kAlpha;      // 半透明
    c.sort = jpov::SortMode::kByDepth;      // 背→前排序
    return c;
}

// ff viewer 可选火纹理（下拉切换）：索引 0 = 程序化 puff；其余 = CC0 火纹理 flipbook。
struct FireTexEntry {
    const char* label;
    const char* file;   // 相对 textures/ 的路径；空 = 程序化 puff
    int tx;
    int ty;
};
inline const FireTexEntry kFireTexes[] = {
    {"程序化 puff",       "",                              8,  8},
    {"火#1  ben1",        "candidates/cand01_ben1.png",    10, 6},
    {"火#2  ben2",        "candidates/cand02_ben2.png",    10, 6},
    {"火#3  ben3",        "candidates/cand03_ben3.png",    10, 6},
    {"火#4  ben4",        "candidates/cand04_ben4.png",    10, 6},
    {"火#5  ben5(原CC0)", "candidates/cand05_ben5.png",    10, 6},
    {"火#6  ben6",        "candidates/cand06_ben6.png",    10, 6},
    {"火#7  ben7",        "candidates/cand07_ben7.png",    10, 6},
    {"火#8  ben8",        "candidates/cand08_ben8.png",    10, 6},
    {"火#9  hw5x5",       "candidates/cand09_hw5x5.png",   5,  5},
    {"火#10 hwrow",       "candidates/cand10_hwrow.png",   25, 1},
};
inline constexpr int kFireTexCount = static_cast<int>(sizeof(kFireTexes) / sizeof(kFireTexes[0]));

// 点状雾查看器渲染核心 App。
class FireFogApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景静态资源（Init() 后装配一次，不在 OneIteration 里重复构造/上传）──
    uint32_t ground_mesh_ = 0;            // 40×40 地面 quad
    jpov::PBRMaterial ground_mat_;        // 灰色高粗糙地面

    // 橡树（高模 glTF）：世界摆放（center/up/front/scale），由主程序装载后传入。
    jpov::GltfObject oak_;
    jpov::Vec3f oak_center_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    jpov::Vec3f oak_up_ = jpov::Vec3f(0.0f, 1.0f, 0.0f);
    jpov::Vec3f oak_front_ = jpov::Vec3f(0.0f, 0.0f, 1.0f);
    float oak_scale_ = 1.0f;

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 光照自由度（沿用 skylight_scene.h；本查看器只固定用白昼值）──
    jpov_skylight::SkyDegrees deg_;

    // ── 点状雾参数（单个团；用户可调）──
    jpov::Vec3f fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);  // box 空间位置（橡树中段）
    jpov::Vec3f fog_size_ = jpov::Vec3f(6.0f, 6.0f, 6.0f);    // box 长宽高（米）
    float fog_radius_ = 1.0f;                                  // box 外侧圆角壳厚度（米）
    float fog_sigma_ = 0.6f;                                   // 消光系数 σ_t（1/m）
    jpov::Color fog_albedo_ = jpov::Color{0.9f, 0.9f, 0.9f, 1.0f};    // 散射色（∈[0,1]）
    jpov::Color fog_emission_ = jpov::Color{0.0f, 0.0f, 0.0f, 1.0f};  // 自发光 ε（HDR，rad/m）
    int fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);

    // ── 一盏点光源（演示 fire_fog 消费点光源：自持 Nxy tile culling）──
    bool point_light_enable_ = true;
    jpov::Vec3f point_light_pos_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    jpov::Color point_light_color_ = jpov::Color{1.0f, 0.85f, 0.6f, 1.0f};
    float point_light_intensity_ = 8.0f;
    float point_light_radius_ = 12.0f;

    // ── god ray ──
    float sun_phase_g_ = 0.7f;         // HG 相位各向异性（太阳/点光源共用）
    float sun_gain_ = 2.0f;            // 太阳项增益

    // ── 广告牌粒子火（主角火；独立于 froxel 雾）──
    // 模拟内核见 src/particles/particle_system.h；渲染走 Renderer 的独立粒子 pass。
    bool fire_enable_ = true;
    jpov::particles::EmitterConfig fire_cfg_ = MakeFireConfig();
    jpov::particles::ParticleEmitter fire_emitter_;
    bool fire_needs_reset_ = true;
    uint32_t fire_texture_id_ = 0;     // 懒建的程序化 flipbook 纹理
    int fire_tex_tiles_ = 8;           // flipbook 网格边长（8×8=64 帧）
    // 面板用影子标量（逐步映射到 fire_cfg_，避免直接改 RangeF 导致 min>max 崩溃）。
    float ui_fire_rate_ = 160.0f;
    float ui_fire_life_ = 1.1f;
    float ui_fire_size_ = 0.5f;
    float ui_fire_speed_ = 1.3f;
    float ui_fire_buoy_ = 1.9f;
    float ui_fire_drag_ = 0.9f;
    float ui_fire_noise_ = 1.5f;
    int ui_fire_blend_ = 0;   // 0=加法 1=半透明
    int ui_fire_align_ = 0;   // 0=view 1=vertical 2=facing 3=velocity

    // ── 黑烟层（半透明广告牌；引擎标准做法）──
    bool smoke_enable_ = true;
    jpov::particles::EmitterConfig smoke_cfg_ = MakeSmokeConfig();
    jpov::particles::ParticleEmitter smoke_emitter_;
    bool smoke_needs_reset_ = true;
    uint32_t smoke_texture_id_ = 0;
    float ui_smoke_rate_ = 0.0f;      // 黑烟基础发射率（走子发射器时保持 0）
    float ui_smoke_size_ = 1.35f;
    float ui_smoke_prob_ = 0.7f;      // 子发射器：每次火焰粒子死亡生成烟的概率
    // 调试开关：用「绝对无硬边」软圆点贴图替换火/烟纹理（判断三角/片片感来源）。
    bool debug_soft_tex_ = false;
    uint32_t soft_fire_tex_ = 0;
    uint32_t soft_smoke_tex_ = 0;
    jpov::particles::Rng sub_rng_{20261015u};   // 子发射器概率（确定性）
    // 可选火纹理（面板下拉「火焰纹理」切换）：fire_choice_ 索引 kFireTexes
    //（0=程序化 puff；1..10=CC0 火纹理 flipbook）。
    int fire_choice_ = 10;   // 默认 #10（我评的 top1）
    std::array<uint32_t, kFireTexCount> fire_choice_tex_{};
    // 临时：用外部指定纹理渲染（候选火纹理评估；env: JPOV_FIRETEX / JPOV_FIRETX / JPOV_FIRETY）。
    bool ov_fire_ = false;
    std::string ov_fire_path_;
    int ov_fire_tx_ = 1;
    int ov_fire_ty_ = 1;
    uint32_t ov_fire_tex_ = 0;

    // ── 场景选择（左上角下拉）──
    //   0 = 体积雾（地面+橡树+点状雾，原 fire_fog 验证场）
    //   1 = 粒子火（一张桌子 + 桌上的广告牌粒子火；光照不变）
    int scene_index_ = 0;

    // ── 粒子火场景的桌面道具（主程序注册后传入；与地面同材质体系）──
    uint32_t table_top_mesh_ = 0;   // 桌面盒
    uint32_t table_leg_mesh_ = 0;   // 桌腿盒
    jpov::PBRMaterial table_mat_;   // 木色材质

    // ── froxel 分辨率（面板下拉；对应 FireFogParams.nz / tile_px）──
    int nz_index_ = 1;        // 0 → Nz=64，1 → Nz=256
    int tile_px_index_ = 1;   // 0 → 8px，1 → 16px，2 → 32px
    // ── froxel z 分布远端（米）；近端固定 0.1（FireFogParams.z_near 默认）。──
    float z_far_ = 2000.0f;

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&FireFogApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 是否绘制雾（headless 拍“无雾基线”时置 false）。
    void SetShowFog(bool show) { show_fog_ = show; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        if (show_panel_) {   // 仅可见窗口交互才消费输入；headless 由外部设 view_
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) {
                scroll = input.scroll_delta;
            }
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }

        // 相机由 view_ 推导（同 skylight viewer；target=原点）。
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 光照：白昼标准天光（单色 ambient），与 skylight viewer 同源。
        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/false);
        cmds->sky     = nl.sky;
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;   // fire_fog pass 要求线性 HDR 域

        // 场景：灰色地面（两场景共用）。
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        if (scene_index_ == 1) {
            // 粒子火场景：一张桌子（桌面 + 四条腿）。
            DrawTable(cmds);
        } else {
            // 体积雾场景：橡树。
            cmds->DrawGltfObject(oak_, oak_center_, oak_up_, oak_front_, oak_scale_);
        }

        // 点光源（fire_fog 消费：自持 Nxy tile culling）——**独立于雾**，稳定场景光照。
        if (point_light_enable_) {
            jpov::PointLight pl;
            pl.position = point_light_pos_;
            pl.color = point_light_color_;
            pl.linear_radius = point_light_radius_;
            pl.intensity = point_light_intensity_;
            cmds->point_lights.push_back(pl);
        }

        // 点状雾（走 fire_fog froxel 体积管线）。仅体积雾场景。
        if (scene_index_ == 0 && show_fog_) {
            jpov::PointFog fog;
            fog.center = fog_center_;
            fog.size = fog_size_;
            fog.radius = fog_radius_;
            fog.sigma = fog_sigma_;
            fog.albedo = fog_albedo_;
            fog.emission = fog_emission_;
            fog.attenuation = static_cast<jpov::FogAttenuation>(fog_attenuation_);
            cmds->point_fogs.push_back(fog);

            jpov::FireFogParams ff;
            ff.sun_phase_g = sun_phase_g_;
            ff.sun_gain = sun_gain_;
            ff.nz = kNzChoices[std::min(std::max(nz_index_, 0), 1)];
            ff.tile_px = kTilePxChoices[std::min(std::max(tile_px_index_, 0), 2)];
            ff.z_far = z_far_;
            cmds->fire_fog = ff;
        }

        // ── 广告牌火/烟：懒加载 flipbook 纹理（需 GL 上下文）──
        // 优先用离线烘的**真火/真烟**纹理（tools/jpov/assets/fire/*.png，build 脚本拷到
        // textures/）；缺失时回退程序化生成，保证不崩。
        if (fire_texture_id_ == 0) {
            const std::string fire_path = FindTexture("textures/fire_flipbook.png");
            if (!fire_path.empty()) {
                fire_texture_id_ = RegisterTexture(fire_path);
                LOG(INFO) << "[particle_fire] fire texture = " << fire_path;
            } else {
                LOG(WARNING) << "[particle_fire] textures/fire_flipbook.png 未找到，"
                                "回退程序化火贴图";
                const std::vector<unsigned char> png =
                    jpov_particle_fire::MakeFireFlipbookPng(fire_tex_tiles_, fire_tex_tiles_,
                                                            128, 20261014u);
                if (!png.empty()) {
                    fire_texture_id_ =
                        RegisterTextureFromMemory(png, "particle_fire_flipbook");
                }
            }
        }
        if (smoke_texture_id_ == 0) {
            const std::string smoke_path = FindTexture("textures/smoke_flipbook.png");
            if (!smoke_path.empty()) {
                smoke_texture_id_ = RegisterTexture(smoke_path);
                LOG(INFO) << "[particle_fire] smoke texture = " << smoke_path;
            } else {
                LOG(WARNING) << "[particle_fire] textures/smoke_flipbook.png 未找到，"
                                "回退火贴图染暗";
                smoke_texture_id_ = fire_texture_id_;
            }
        }
        // 选中的火 flipbook 纹理（懒加载 + 缓存）。
        if (fire_choice_ >= 0 && fire_choice_ < kFireTexCount &&
            kFireTexes[fire_choice_].file[0] != '\0' &&
            fire_choice_tex_[fire_choice_] == 0) {
            const std::string rel = std::string("textures/") + kFireTexes[fire_choice_].file;
            const std::string p = FindTexture(rel.c_str());
            if (!p.empty()) {
                fire_choice_tex_[fire_choice_] = RegisterTexture(p);
                LOG(INFO) << "[particle_fire] fire choice tex = " << p;
            } else {
                LOG(WARNING) << "[particle_fire] 未找到 " << rel << "（回退程序化 puff）";
            }
        }
        // 临时覆盖纹理（候选评估）。
        if (ov_fire_ && ov_fire_tex_ == 0 && !ov_fire_path_.empty()) {
            if (FileExists(ov_fire_path_.c_str())) {
                ov_fire_tex_ = RegisterTexture(ov_fire_path_);
                LOG(INFO) << "[particle_fire] override fire tex = " << ov_fire_path_;
            }
        }
        // 调试：超软圆点贴图（绝对无硬边），用于排查三角/片片感来源。
        if (debug_soft_tex_) {
            if (soft_fire_tex_ == 0) {
                const std::vector<unsigned char> png =
                    jpov_particle_fire::MakeSoftBlobPng(fire_tex_tiles_, fire_tex_tiles_,
                                                        128, /*warm=*/true);
                if (!png.empty()) {
                    soft_fire_tex_ = RegisterTextureFromMemory(png, "soft_fire_blob");
                }
            }
            if (soft_smoke_tex_ == 0) {
                const std::vector<unsigned char> png =
                    jpov_particle_fire::MakeSoftBlobPng(fire_tex_tiles_, fire_tex_tiles_,
                                                        128, /*warm=*/false);
                if (!png.empty()) {
                    soft_smoke_tex_ = RegisterTextureFromMemory(png, "soft_smoke_blob");
                }
            }
        }
        // ── 火焰层（先推进，拿到「死亡位置」给烟的子发射器）──
        std::vector<jpov::Vec3f> flame_deaths;
        std::vector<jpov::ParticleInstance> finst;
        const float dt = 1.0f / static_cast<float>(kViewerFps);
        const bool do_fire = (scene_index_ == 1 && fire_enable_);
        // 是否用「选中的 flipbook」（而非程序化 puff / 外部覆盖）。
        const bool choice_flip =
            !ov_fire_ && fire_choice_ >= 0 && fire_choice_ < kFireTexCount &&
            kFireTexes[fire_choice_].file[0] != '\0';
        if (do_fire) {
            // 面板影子标量 → 配置（RangeF 两端一起设，避免 min>max）。
            fire_cfg_.rate = ui_fire_rate_;
            fire_cfg_.lifetime = {ui_fire_life_ * 0.7f, ui_fire_life_};
            fire_cfg_.size_start = {ui_fire_size_ * 0.75f, ui_fire_size_};
            fire_cfg_.speed = {ui_fire_speed_ * 0.6f, ui_fire_speed_};
            fire_cfg_.gravity = {0.0f, ui_fire_buoy_, 0.0f};
            fire_cfg_.drag = ui_fire_drag_;
            fire_cfg_.noise_amp = ui_fire_noise_;
            fire_cfg_.blend = (ui_fire_blend_ == 0) ? jpov::BlendMode::kAdditive
                                                   : jpov::BlendMode::kAlpha;
            fire_cfg_.align = static_cast<jpov::AlignMode>(ui_fire_align_);
            // 加法层无顺序依赖不排序；半透明层需背→前排序。
            fire_cfg_.sort = (ui_fire_blend_ == 0) ? jpov::SortMode::kNone
                                                   : jpov::SortMode::kByDepth;
            // 贴图/帧设置：外部覆盖 / 选中的 flipbook（kFps 随机相位） / 程序化 puff（kLifetime）。
            if (ov_fire_) {
                fire_cfg_.tiles_x = ov_fire_tx_;
                fire_cfg_.tiles_y = ov_fire_ty_;
                fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
                fire_cfg_.sheet_fps = 24.0f;
                fire_cfg_.start_frame_random = true;
                fire_cfg_.sheet_cycles = 1.0f;
            } else if (choice_flip) {
                fire_cfg_.tiles_x = kFireTexes[fire_choice_].tx;
                fire_cfg_.tiles_y = kFireTexes[fire_choice_].ty;
                fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kFps;
                fire_cfg_.sheet_fps = 24.0f;
                fire_cfg_.start_frame_random = true;
                fire_cfg_.sheet_cycles = 1.0f;
            } else {
                fire_cfg_.tiles_x = fire_tex_tiles_;
                fire_cfg_.tiles_y = fire_tex_tiles_;
                fire_cfg_.sheet_time_mode = jpov::particles::SheetTimeMode::kLifetime;
                fire_cfg_.start_frame_random = false;
                fire_cfg_.sheet_cycles = 1.0f;
                fire_cfg_.frame_curve = jpov::particles::MakeDefaultLinearFrameCurve();
            }

            if (fire_needs_reset_) {
                fire_emitter_.SetConfig(fire_cfg_);
                fire_emitter_.Reset(20261014u);
                fire_needs_reset_ = false;
            }
            fire_emitter_.SetConfig(fire_cfg_);
            fire_emitter_.Update(dt, &finst, &flame_deaths);
        }

        // ── 黑烟层（alpha；先画，火在其上加法叠加）──
        // 标准做法：烟**不自己从根部发射**，而是由火焰粒子**死亡时**在火苗顶端生成（子发射器）。
        if (scene_index_ == 1 && smoke_enable_) {
            smoke_cfg_.rate = ui_smoke_rate_;
            smoke_cfg_.size_start = {ui_smoke_size_ * 0.65f, ui_smoke_size_};
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
            if (!sinst.empty()) {
                jpov::ParticleRenderCommand spc;
                spc.particles = std::move(sinst);
                spc.texture_id = debug_soft_tex_ ? soft_smoke_tex_ : smoke_texture_id_;
                spc.tiles_x = smoke_cfg_.tiles_x;
                spc.tiles_y = smoke_cfg_.tiles_y;
                spc.flipbook = smoke_cfg_.flipbook;
                spc.align = smoke_cfg_.align;
                spc.stretched = smoke_cfg_.stretched;
                spc.stretch_scale = smoke_cfg_.stretch_scale;
                spc.blend = smoke_cfg_.blend;
                spc.sort = smoke_cfg_.sort;
                cmds->DrawParticles(std::move(spc));
            }
        }

        // ── 火焰命令（后画，加法叠在烟上）──
        if (do_fire && !finst.empty()) {
            jpov::ParticleRenderCommand pc;
            pc.particles = std::move(finst);
            pc.texture_id = ov_fire_ ? ov_fire_tex_
                          : (choice_flip ? fire_choice_tex_[fire_choice_]
                          : (debug_soft_tex_ ? soft_fire_tex_ : fire_texture_id_));
            pc.tiles_x = fire_cfg_.tiles_x;
            pc.tiles_y = fire_cfg_.tiles_y;
            pc.flipbook = fire_cfg_.flipbook;
            pc.align = fire_cfg_.align;
            pc.stretched = fire_cfg_.stretched;
            pc.stretch_scale = fire_cfg_.stretch_scale;
            pc.blend = fire_cfg_.blend;
            pc.sort = fire_cfg_.sort;
            cmds->DrawParticles(std::move(pc));
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        FireFogApp* app = static_cast<FireFogApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 文件是否存在（纹理可选加载的探测；不抛异常）。
    static bool FileExists(const char* path) {
        std::ifstream f(path);
        return f.good();
    }

    // 可执行文件所在目录（与 cwd 解耦：无论从哪个目录启动都能找到 exe 旁 textures/）。
    static std::string ExeDir() {
#ifndef _WIN32
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            const std::string p(buf);
            const size_t slash = p.find_last_of('/');
            if (slash != std::string::npos) {
                return p.substr(0, slash);
            }
        }
#endif
        return std::string();
    }

    // 按「exe 旁 → cwd」顺序找一个 flipbook 纹理；找不到返回空串。
    static std::string FindTexture(const char* rel) {
        const std::string exe_dir = ExeDir();
        if (!exe_dir.empty()) {
            const std::string p = exe_dir + "/" + rel;
            if (FileExists(p.c_str())) {
                return p;
            }
        }
        if (FileExists(rel)) {
            return std::string(rel);
        }
        return std::string();
    }

    // 画粒子火场景的桌子（桌面 + 四条腿，均用轴对齐盒 mesh）。
    // Pre-condition: table_top_mesh_/table_leg_mesh_ 已注册。
    void DrawTable(jpov::RenderCommandList* cmds) {
        CHECK(cmds != nullptr);
        const jpov::Vec3f up = {0.0f, 1.0f, 0.0f};
        const jpov::Vec3f front = {0.0f, 0.0f, 1.0f};
        // 桌面（半高 0.04，中心 y=0.74 ⇒ 顶面 y=0.78）。
        cmds->DrawObject3D(table_top_mesh_, table_mat_,
                           /*center*/ {0.0f, 0.74f, 0.0f}, up, front);
        // 四条腿（半高 0.35，中心 y=0.35 ⇒ 0..0.70）。
        const float lx = 0.70f;
        const float lz = 0.36f;
        const float leg[4][2] = {{-lx, -lz}, {lx, -lz}, {-lx, lz}, {lx, lz}};
        for (int i = 0; i < 4; ++i) {
            cmds->DrawObject3D(table_leg_mesh_, table_mat_,
                               /*center*/ {leg[i][0], 0.35f, leg[i][1]}, up, front);
        }
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float colw = 0.235f * w;        // 每列滑条宽度（4 列）
        const float x0 = 0.01f * w;
        const float x1 = 0.255f * w;
        const float x2 = 0.50f * w;
        const float x3 = 0.745f * w;
        const float kRowH = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom = 16.0f;
        const int kRows = 12;
        const float top = h - kBottom
                        - (static_cast<float>(kRows) * kRowH
                           + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int col, int i) {
            const float x = (col == 0) ? x0
                          : ((col == 1) ? x1 : ((col == 2) ? x2 : x3));
            return jpov::UiRect{
                {x, top + static_cast<float>(i) * (kRowH + kSpacing)}, {colw, kRowH}};
        };

        // 左上角：场景选择下拉（不随面板列布局；两场景共用一套光照）。
        {
            const std::vector<const char*> items = {"体积雾（橡树）", "粒子火（桌面）"};
            ui_.Combo("场景", &scene_index_, items,
                      jpov::UiRect{{x0, 8.0f}, {0.26f * w, 26.0f}});
        }
        // 顶部：火焰纹理下拉（放顶部才有空间展开；仅粒子火场景有意义）。
        {
            std::vector<const char*> items;
            items.reserve(static_cast<size_t>(kFireTexCount));
            for (int i = 0; i < kFireTexCount; ++i) {
                items.push_back(kFireTexes[i].label);
            }
            ui_.Combo("火焰纹理", &fire_choice_, items,
                      jpov::UiRect{{x0, 40.0f}, {0.30f * w, 26.0f}});
        }

        if (scene_index_ == 0) {
        // ── 列 0：box 几何（位置 + 长宽高 + 圆角壳厚度）+ 消光 + froxel 分辨率 ──
        ui_.SliderFloat("中心 x", &fog_center_.x(), row(0, 0), -10.0f, 10.0f, 2);
        ui_.SliderFloat("中心 y", &fog_center_.y(), row(0, 1), 0.0f, 10.0f, 2);
        ui_.SliderFloat("中心 z", &fog_center_.z(), row(0, 2), -10.0f, 10.0f, 2);
        ui_.SliderFloat("尺寸 x m", &fog_size_.x(), row(0, 3), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 y m", &fog_size_.y(), row(0, 4), 0.2f, 12.0f, 2);
        ui_.SliderFloat("尺寸 z m", &fog_size_.z(), row(0, 5), 0.2f, 12.0f, 2);
        ui_.SliderFloat("消光 σ 1/m", &fog_sigma_, row(0, 6), 0.0f, 4.0f, 3);
        ui_.SliderFloat("圆角半径 m", &fog_radius_, row(0, 7), 0.0f, 6.0f, 2);
        {
            const std::vector<const char*> items = {"Nz=64", "Nz=256"};
            ui_.Combo("z 切片 Nz", &nz_index_, items, row(0, 8));
        }
        {
            const std::vector<const char*> items = {"tile=8px", "tile=16px", "tile=32px"};
            ui_.Combo("Nxy 单元 tile", &tile_px_index_, items, row(0, 9));
        }
        ui_.SliderFloat("z 远端 far(m)", &z_far_, row(0, 10), 50.0f, 4000.0f, 0);

        // ── 列 1：剖面 + 散射色 albedo + 自发光 emission + god ray ──
        {
            const std::vector<const char*> items = {
                "均匀 kUniform", "线性 kLinear", "二次 kQuadratic", "指数 kExponential"};
            ui_.Combo("衰减剖面", &fog_attenuation_, items, row(1, 0));
        }
        ui_.SliderFloat("散射 R", &fog_albedo_.r, row(1, 1), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 G", &fog_albedo_.g, row(1, 2), 0.0f, 1.0f, 2);
        ui_.SliderFloat("散射 B", &fog_albedo_.b, row(1, 3), 0.0f, 1.0f, 2);
        ui_.SliderFloat("自发光 R", &fog_emission_.r, row(1, 4), 0.0f, 4.0f, 2);
        ui_.SliderFloat("自发光 G", &fog_emission_.g, row(1, 5), 0.0f, 4.0f, 2);
        ui_.SliderFloat("自发光 B", &fog_emission_.b, row(1, 6), 0.0f, 4.0f, 2);
        ui_.SliderFloat("光柱 相位 g", &sun_phase_g_, row(1, 7), 0.0f, 0.9f, 2);
        ui_.SliderFloat("光柱 增益", &sun_gain_, row(1, 8), 0.0f, 4.0f, 2);

        // ── 列 2：点光源 + 太阳角度 ──
        ui_.Checkbox("点光源", &point_light_enable_, row(2, 0));
        ui_.SliderFloat("点光 x", &point_light_pos_.x(), row(2, 1), -10.0f, 10.0f, 2);
        ui_.SliderFloat("点光 y", &point_light_pos_.y(), row(2, 2), 0.0f, 10.0f, 2);
        ui_.SliderFloat("点光 z", &point_light_pos_.z(), row(2, 3), -10.0f, 10.0f, 2);
        ui_.SliderFloat("点光 亮度", &point_light_intensity_, row(2, 4), 0.0f, 40.0f, 1);
        ui_.SliderFloat("点光 半径", &point_light_radius_, row(2, 5), 0.5f, 40.0f, 1);
        ui_.SliderFloat("太阳仰角 °", &deg_.sun_elev_deg, row(2, 6), 0.0f, 90.0f, 0);
        ui_.SliderFloat("太阳方位角 °", &deg_.sun_azim_deg, row(2, 7), 0.0f, 360.0f, 0);
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(2, 8), 0.0f, 8.0f, 1);
        } else {
            // ── 粒子火场景面板 ──
            ui_.Checkbox("粒子火", &fire_enable_, row(0, 0));
            ui_.SliderFloat("发射率 /s", &ui_fire_rate_, row(0, 1), 0.0f, 200.0f, 0);
            ui_.SliderFloat("寿命 s", &ui_fire_life_, row(0, 2), 0.2f, 4.0f, 2);
            ui_.SliderFloat("尺寸 m", &ui_fire_size_, row(0, 3), 0.1f, 3.0f, 2);
            ui_.SliderFloat("速度 m/s", &ui_fire_speed_, row(0, 4), 0.0f, 5.0f, 2);
            ui_.SliderFloat("浮力", &ui_fire_buoy_, row(0, 5), -2.0f, 8.0f, 2);
            ui_.SliderFloat("阻尼", &ui_fire_drag_, row(0, 6), 0.0f, 4.0f, 2);
            ui_.Checkbox("湍流", &fire_cfg_.noise_enabled, row(0, 7));
            ui_.SliderFloat("湍流强度", &ui_fire_noise_, row(0, 8), 0.0f, 5.0f, 2);
            ui_.Checkbox("flipbook", &fire_cfg_.flipbook, row(0, 9));
            {
                const std::vector<const char*> items = {"加法", "半透明"};
                ui_.Combo("混合", &ui_fire_blend_, items, row(1, 0));
            }
            {
                const std::vector<const char*> items = {
                    "view", "vertical", "facing", "velocity"};
                ui_.Combo("朝向", &ui_fire_align_, items, row(1, 1));
            }
            ui_.Checkbox("沿速度拉伸", &fire_cfg_.stretched, row(1, 2));
            // 黑烟层
            ui_.Checkbox("黑烟", &smoke_enable_, row(1, 4));
            ui_.SliderFloat("黑烟基础率 /s", &ui_smoke_rate_, row(1, 5), 0.0f, 60.0f, 0);
            ui_.SliderFloat("黑烟尺寸 m", &ui_smoke_size_, row(1, 6), 0.2f, 2.5f, 2);
            ui_.SliderFloat("烟概率", &ui_smoke_prob_, row(1, 7), 0.0f, 1.0f, 2);
            ui_.Checkbox("软贴图(debug)", &debug_soft_tex_, row(1, 8));
        }
    }

    bool show_panel_ = true;
    bool show_fog_ = true;
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_fire_fog

#endif  // JPOV_DEMO_FIRE_FOG_VIEWER_APP_H_

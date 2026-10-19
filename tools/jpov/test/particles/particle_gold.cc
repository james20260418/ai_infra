// JPOV 粒子火 Gold Test —— 覆盖 火 + 烟 + 火星（ember）三层
//
// 渲染固定场景（固定种子、固定 240 帧预演、固定相机/天光），
// 与 git 管理的 PNG 做**逐字节**比较。
//
// 用法：
//   bazel run //tools/jpov/test/particles:particle_gold_generator   # 重出 gold
//   bazel test //tools/jpov/test/particles:particle_gold_test       # 回归
//
// 覆盖点：
//   * 火焰：12×6 成对形变 atlas（V7.1）+ 自旋 + 温度调色
//   * 烟：danis 重绘 12×6 图集，火焰死亡点 spawn，kAlpha over
//   * 火星：纯色加法小点，数量/大小固定

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/demo/fire_palette.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/particles/particle_system.h"
#include "tools/jpov/test/test_utils.h"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr float kDt = 1.0f / 60.0f;
constexpr int kPreRoll = 240;

using jpov::particles::EmitterConfig;
using jpov::particles::ParticleEmitter;
using jpov::ParticleInstance;

EmitterConfig MakeFireConfig() {
    EmitterConfig c;
    c.origin = {0.0f, 0.26f, 0.0f};
    c.rate = 86.5f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.10f;
    c.cone_half_angle_deg = 32.0f;
    c.lifetime = {0.75f, 1.10f};
    c.speed = {0.20f, 0.45f};
    c.size_start = {0.108f, 0.18f};
    c.rotation = {0.0f, 6.2831853f};
    c.rotation_speed = {-1.4f, 1.4f};
    c.rotation_speed_decay = 0.85f;
    c.gravity = {0.0f, 0.55f, 0.0f};
    c.drag = 1.0f;
    c.noise_enabled = true;
    c.noise_amp = 0.9f;
    c.noise_freq = 1.8f;
    c.use_color_over_life = false;
    c.use_size_over_life = false;
    c.flipbook = true;
    c.tiles_x = 12;
    c.tiles_y = 6;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kPairMorph;
    c.pair_len = 12;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAlpha;
    return c;
}

EmitterConfig MakeSmokeConfig() {
    EmitterConfig c;
    c.origin = {0.0f, 0.51f, 0.0f};
    c.rate = 0.0f;
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
    c.size_curve.keys = {{0.0f, 0.90f}, {0.5f, 2.00f}, {1.0f, 3.40f}};
    c.flipbook = true;
    c.tiles_x = 12;
    c.tiles_y = 6;
    c.sheet_time_mode = jpov::particles::SheetTimeMode::kPairMorph;
    c.pair_len = 12;
    c.start_frame_random = true;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAlpha;
    return c;
}

EmitterConfig MakeEmberConfig() {
    EmitterConfig c;
    c.origin = {0.0f, 0.43f, 0.0f};
    c.rate = 20.0f;
    c.shape = jpov::particles::ShapeKind::kCone;
    c.radius = 0.10f;
    c.cone_half_angle_deg = 35.0f;
    c.lifetime = {0.6f, 1.4f};
    c.speed = {0.25f, 0.70f};
    c.size_start = {0.006f, 0.010f};
    c.rotation = {0.0f, 6.2831853f};
    c.gravity = {0.0f, 0.25f, 0.0f};
    c.drag = 0.70f;
    c.noise_enabled = true;
    c.noise_amp = 1.2f;
    c.noise_freq = 2.2f;
    c.use_color_over_life = true;
    jpov::particles::Gradient g;
    g.keys = {
        {0.00f, jpov::Color{1.80f, 1.40f, 0.60f, 0.00f}},
        {0.10f, jpov::Color{2.00f, 1.50f, 0.55f, 1.00f}},
        {0.45f, jpov::Color{1.40f, 0.55f, 0.08f, 0.90f}},
        {0.75f, jpov::Color{0.80f, 0.12f, 0.02f, 0.55f}},
        {1.00f, jpov::Color{0.25f, 0.02f, 0.00f, 0.00f}},
    };
    c.color_over_life = g;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.60f}, {0.30f, 1.00f}, {1.0f, 0.30f}};
    c.flipbook = false;
    c.texture_id = 0;
    c.align = jpov::AlignMode::kView;
    c.blend = jpov::BlendMode::kAdditive;
    return c;
}

class ParticleGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    void Setup() {
        fire_tex_ = RegisterTexture(jpov::ResolveResourcePath(
            "tools/jpov/assets/fire/mp/flames_pairs_v71_12x6.png"));
        smoke_tex_ = RegisterTexture(jpov::ResolveResourcePath(
            "tools/jpov/assets/smoke/danis_repaint_12x6.png"));
        fuel_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(0.5f, 0.5f, 0.5f));
        fuel_mat_ = jpov::PBRMaterial::SolidColor({0.18f, 0.10f, 0.06f, 1.0f});
        CHECK_NE(fire_tex_, 0u);
        CHECK_NE(smoke_tex_, 0u);

        fire_ = ParticleEmitter(MakeFireConfig());
        smoke_ = ParticleEmitter(MakeSmokeConfig());
        ember_ = ParticleEmitter(MakeEmberConfig());
        fire_.Reset(20261019u);
        smoke_.Reset(20261015u);
        ember_.Reset(20261019u);

        for (int i = 0; i < kPreRoll; ++i) {
            Step();
        }
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;
        Step();

        cmds->camera.fbo_3d_width_ = kWidth;
        cmds->camera.fbo_3d_height_ = kHeight;
        cmds->camera.position = {0.0f, 0.55f, 2.2f};
        cmds->camera.target = {0.0f, 0.42f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 50.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;

        cmds->sky = jpov::CreateDefaultSkyCommand(
            2.0f, jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
            jpov::Vec3f{0.45f, 0.35f, 0.82f},
            jpov::Vec3f{-0.45f, -0.35f, -0.82f});
        jpov::DirectionalLight sun;
        sun.direction = {-0.45f, -0.35f, -0.82f};
        sun.color = {1.0f, 0.95f, 0.90f, 1.0f};
        sun.intensity = 2.5f;
        cmds->sun = sun;
        jpov::AmbientLight ambient;
        ambient.color = {0.20f, 0.25f, 0.30f, 1.0f};
        ambient.intensity = 0.5f;
        cmds->ambient = ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(fuel_mesh_, fuel_mat_,
                           {0.0f, 0.125f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                           /*scale=*/0.25f);

        // 火焰：温度 → 颜色（与 viewer 的 Cinematic 调色一致）
        std::vector<ParticleInstance> fire = fire_inst_;
        for (ParticleInstance& p : fire) {
            const float age = std::clamp(p.age01, 0.0f, 1.0f);
            const float tau = std::clamp(1.0f - age, 0.0f, 1.0f);
            jpov::Color c = jpov_particle::FirePaletteColor(1, tau);
            const float fade_in = std::min(1.0f, age / 0.05f);
            const float fade_out = 1.0f - std::max(0.0f, (age - 0.72f) / 0.28f);
            c.a = std::clamp(fade_in * fade_out, 0.0f, 1.0f);
            p.color = c;
        }
        if (!fire.empty()) {
            jpov::ParticleRenderCommand pc;
            pc.particles = std::move(fire);
            pc.texture_id = fire_tex_;
            pc.tiles_x = 12;
            pc.tiles_y = 6;
            pc.flipbook = true;
            pc.align = jpov::AlignMode::kView;
            pc.blend = jpov::BlendMode::kAlpha;
            pc.frame_blend = true;
            cmds->DrawParticles(std::move(pc));
        }

        // 烟：密度归一化系数 0.75（与 viewer 的 danis 重绘一致）
        std::vector<ParticleInstance> smoke = smoke_inst_;
        for (ParticleInstance& p : smoke) {
            p.color.a = std::clamp(p.color.a * 0.75f, 0.0f, 1.0f);
        }
        if (!smoke.empty()) {
            jpov::ParticleRenderCommand sc;
            sc.particles = std::move(smoke);
            sc.texture_id = smoke_tex_;
            sc.tiles_x = 12;
            sc.tiles_y = 6;
            sc.flipbook = true;
            sc.align = jpov::AlignMode::kView;
            sc.blend = jpov::BlendMode::kAlpha;
            sc.frame_blend = true;
            cmds->DrawParticles(std::move(sc));
        }

        // 火星：纯色加法
        if (!ember_inst_.empty()) {
            jpov::ParticleRenderCommand ec;
            ec.particles = ember_inst_;
            ec.texture_id = 0;
            ec.flipbook = false;
            ec.align = jpov::AlignMode::kView;
            ec.blend = jpov::BlendMode::kAdditive;
            cmds->DrawParticles(std::move(ec));
        }
    }

private:
    void Step() {
        std::vector<jpov::Vec3f> deaths;
        std::vector<ParticleInstance> fire;
        fire_.Update(kDt, &fire, &deaths);

        for (const jpov::Vec3f& d : deaths) {
            if (sub_rng_.Next01() < 0.55f) {
                smoke_.SpawnAt(d);
            }
        }
        std::vector<ParticleInstance> smoke;
        smoke_.Update(kDt, &smoke);
        std::vector<ParticleInstance> ember;
        ember_.Update(kDt, &ember);

        fire_inst_ = std::move(fire);
        smoke_inst_ = std::move(smoke);
        ember_inst_ = std::move(ember);
    }

    uint32_t fire_tex_ = 0;
    uint32_t smoke_tex_ = 0;
    uint32_t fuel_mesh_ = 0;
    jpov::PBRMaterial fuel_mat_;
    ParticleEmitter fire_;
    ParticleEmitter smoke_;
    ParticleEmitter ember_;
    jpov::particles::Rng sub_rng_{20261015u};
    std::vector<ParticleInstance> fire_inst_;
    std::vector<ParticleInstance> smoke_inst_;
    std::vector<ParticleInstance> ember_inst_;
};

std::string GoldPath() {
    return jpov::GetTestDataDir() +
           "/particles/particle_fire_smoke_ember_1280x720.png";
}

void RenderTo(const std::string& out_path) {
    JPOV::Config cfg;
    cfg.title = "JPOV particle gold test";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    ParticleGoldApp app(cfg);
    app.Init();
    app.Setup();

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

}  // namespace

int main(int argc, char** argv) {
    const bool generate = (argc > 1 && std::strcmp(argv[1], "--generate") == 0);
    if (generate) {
        const std::string path = GoldPath();
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());
        RenderTo(path);
        LOG(INFO) << "gold written: " << path;
        return 0;
    }

    const std::string expected_path = GoldPath();
    std::vector<uint8_t> expected;
    if (!jpov::ReadFileBytes(expected_path, &expected)) {
        LOG(FATAL) << "missing gold: " << expected_path;
    }

    const std::string out_dir = jpov::GetOutputDir() + "jpov_particle_gold_test/";
    const std::string out_path = out_dir + "rendered.png";
    RenderTo(out_path);

    std::vector<uint8_t> rendered;
    if (!jpov::ReadFileBytes(out_path, &rendered)) {
        LOG(FATAL) << "missing render: " << out_path;
    }
    if (rendered != expected) {
        LOG(ERROR) << "gold mismatch: rendered=" << rendered.size()
                   << " bytes, expected=" << expected.size() << " bytes";
        return 1;
    }
    LOG(INFO) << "gold match: " << rendered.size() << " bytes";
    return 0;
}

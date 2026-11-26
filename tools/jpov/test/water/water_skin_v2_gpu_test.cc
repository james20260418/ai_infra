// JPOV 水皮 V2（单像素多法线采样）GPU 测试 —— 锁两条判据
//
// ① **平面等价性**（`V2FlatWaterMatchesV1Exactly`，最强的正确性判据）：
//    无波浪时 S 个样本落在同一平面上、法线都等于 (0,1,0) ⇒ 平均 = 那一份
//    ⇒ V2 必须与 V1 **逐像素相同**（着色公式 / 菲涅尔 / WBO 三槽语义 / 深度 /
//    透射率任何一处写歪都会红）。容差只留 float 求和的最后几位。
// ② **采样是活的**（`V2WithWavesDiffersAndRespondsToSampleCount`）：
//    有波浪时 V2 ≠ V1，且 S=1 ≠ S=8（否则「多法线」根本没接上）；
//    同时所有输出必须有限、α ∈ [0,1]（相位/原点/求交出 NaN 会立刻被抓到）。
//
// 负向验证（提交前实测）：
//   · 把 ShadeAboveV2 的平均换成「平均法向再着色一次」⇒ ② 红（S=1 与 S=8 变成同一个值）；
//   · 把 S 恒传 1 ⇒ ② 的 S=1/S=8 那条红；
//   · 把 V2 的 U → 平均前不除 S（漏除）⇒ ① 红（颜色整体放大 S 倍）。
//
// 合成输入，不需要场景/物体/相机矩阵：水皮的输入只有「env + 场景深度 + 相机 + 波浪谱」。

#define GL_GLEXT_PROTOTYPES

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/lighting/ggx_specular.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_skin_renderer.h"
#include "tools/jpov/src/water/water_skin_v2_renderer.h"

namespace {

using jpov::Camera;
using jpov::Vec3f;
using jpov::WaterSkinRenderer;
using jpov::WaterSkinV2Renderer;

constexpr int kWidth = 48;
constexpr int kHeight = 32;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;

// ───────────────────────── GL context（headless）─────────────────────────

class NullApp : public JPOV {
public:
    using JPOV::JPOV;
    void OneIteration(int64_t, const jpov::InputSnapshot&, const jpov::WindowInfo&,
                      jpov::RenderCommandList*) override {}
};

struct GlEnv {
    std::unique_ptr<NullApp> app;

    static GlEnv& Get() {
        static GlEnv env;
        return env;
    }

    void Ensure() {
        if (app) {
            return;
        }
        JPOV::Config cfg;
        cfg.title = "Water Skin V2 GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// 天光探针：上半球同一个色，下半球（−Y）黑（= 地色）。理由见 water_skin_gpu_test.cc。
constexpr float kSkyR = 0.50f;
constexpr float kSkyG = 0.60f;
constexpr float kSkyB = 1.00f;
constexpr float kEnvScale = 1.0f;

unsigned int MakeEnvCube() {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    const float sky[4] = {kSkyR, kSkyG, kSkyB, 1.0f};
    const float ground[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int f = 0; f < 6; ++f) {
        const GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + f;
        const bool is_negative_y = (face == GL_TEXTURE_CUBE_MAP_NEGATIVE_Y);
        glTexImage2D(face, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT,
                     is_negative_y ? ground : sky);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return tex;
}

unsigned int MakeDepthTexture() {
    // 全 1.0 = 背景（没有不透明几何挡住水面）。
    const std::vector<float> values(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kWidth, kHeight, 0, GL_RED, GL_FLOAT,
                 values.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

std::vector<float> ReadRgba(unsigned int tex) {
    unsigned int fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    std::vector<float> out(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return out;
}

std::vector<float> ReadRed(unsigned int tex) {
    unsigned int fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    std::vector<float> out(static_cast<size_t>(kWidth) * kHeight, 0.0f);
    glReadPixels(0, 0, kWidth, kHeight, GL_RED, GL_FLOAT, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return out;
}

// 一次渲染的全部输入（两版 renderer 共用）。
struct SceneInputs {
    Camera cam;
    unsigned int depth_tex = 0;
    float level_y = 0.0f;
    float ior = 1.333f;
    bool has_sun = false;
    Vec3f sun_toward = {0.0f, 1.0f, 0.0f};
    Vec3f sun_irradiance = {0.0f, 0.0f, 0.0f};
    const jpov::WaterWaves* waves = nullptr;
    float wave_time_s = 1.3f;
    float wave_depth_m = 300.0f;
    int wave_debug = 0;
};

WaterSkinRenderer::Inputs ToInputs(const SceneInputs& s, unsigned int cube) {
    WaterSkinRenderer::Inputs in;
    in.camera = s.cam;
    in.full_w = kWidth;
    in.full_h = kHeight;
    in.scene_depth_tex = s.depth_tex;
    in.env_cube_tex = cube;
    in.env_scale = kEnvScale;
    in.level_y = s.level_y;
    in.ior = s.ior;
    in.medium = nullptr;   // 水上：不需要介质
    in.has_sun = s.has_sun;
    in.sun_toward = s.sun_toward;
    in.sun_irradiance = s.sun_irradiance;
    // 测试里关「太阳的像素足迹带宽」（着色器的 dFdx 无法在 CPU 复现；V2 本来就没有它）。
    in.sun_footprint = 0.0f;
    in.waves = s.waves;
    in.wave_time_s = s.wave_time_s;
    in.wave_depth_m = s.wave_depth_m;
    in.wave_debug = s.wave_debug;
    return in;
}

Vec3f Normalized(const Vec3f& v) {
    const float n = v.Norm();
    if (!(n > 1.0e-8f)) {
        return Vec3f{0.0f, 1.0f, 0.0f};
    }
    return Vec3f{v.x() / n, v.y() / n, v.z() / n};
}

// 3 主波 + 8 细节短波：主波决定几何/大法线，细节波决定「亚像素结构」——
// V1 会按足迹把细节波截掉，V2 必须**全部采样**，这正是两版应有的差异来源。
//
// ⚠️ `v2` 是「谱口径」标记：V1 pass 要 false、V2 pass 要 true（V2 renderer 会 CHECK）。
//    天球折射读同一个字段，所以测试里成对使用。
jpov::WaterWaves MakeTestWaves(bool v2) {
    jpov::WaterWaves w;
    w.v2 = v2;
    w.enabled = true;
    w.num_waves = 3;
    w.waves[0] = jpov::WaterWave{1.0f, 0.0f, 9.0f, 0.35f, 0.2f};
    w.waves[1] = jpov::WaterWave{0.4f, 0.9f, 5.0f, 0.18f, 1.4f};
    w.waves[2] = jpov::WaterWave{-0.7f, 0.7f, 3.0f, 0.08f, 2.2f};
    w.num_detail_waves = 8;
    for (int i = 0; i < 8; ++i) {
        jpov::WaterWave d;
        const float ang = 0.7f * static_cast<float>(i);
        d.dir_x = std::sin(ang);
        d.dir_z = std::cos(ang);
        d.wavelength_m = 1.2f - 0.1f * static_cast<float>(i);   // 0.5~1.2 m
        d.amplitude_m = 0.02f;
        d.phase_rad = 0.9f * static_cast<float>(i);
        w.detail_waves[i] = d;
    }
    return w;
}

struct Fixture {
    GlEnv& env = GlEnv::Get();
    jpov::ShaderManager shader_mgr;
    WaterSkinRenderer v1;
    WaterSkinV2Renderer v2;
    unsigned int cube = 0;

    void SetUp() {
        env.Ensure();
        cube = MakeEnvCube();
        v1.Init(&shader_mgr);
        v2.Init(&shader_mgr);
    }
    void TearDown() {
        v1.Finalize();
        v2.Finalize();
        if (cube != 0) {
            glDeleteTextures(1, &cube);
            cube = 0;
        }
    }

    // 用 V1 渲一张；readback 由调用方用 v1.layer() 做。
    void DrawV1(const SceneInputs& s) {
        const WaterSkinRenderer::Inputs in = ToInputs(s, cube);
        v1.Begin(kWidth, kHeight);
        v1.Draw(in);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void DrawV2(const SceneInputs& s, int sample_count) {
        const WaterSkinV2Renderer::Inputs in = ToInputs(s, cube);
        v2.Begin(kWidth, kHeight);
        v2.Draw(in, sample_count);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

// ① 无波浪（纯平面）：V2 = V1 逐像素相同。
TEST(WaterSkinV2Gpu, V2FlatWaterMatchesV1Exactly) {
    Fixture f;
    f.SetUp();
    SceneInputs s;
    s.cam.position = {0.0f, 6.0f, 9.0f};
    s.cam.target = {0.0f, 0.0f, 0.0f};
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.0f;
    // 带太阳：日盘像（GGX）也必须与 V1 逐像素一致 —— V2 的 a2 里没有足迹项，
    // 平面情况下两者都只剩「盘角带宽」，正好对齐。
    // ⚠️ 这条等价性只在 **MSS = 0**（无波浪 ⇒ 风速 0 ⇒ Cox–Munk = 0）时成立：
    //    一旦给风，V2 的太阳 lobe 会比 V1 宽 2σ²，这条就会红 —— 那是预期，不是回归。
    s.has_sun = true;
    s.sun_toward = Normalized(Vec3f{0.35f, 0.85f, 0.4f});
    s.sun_irradiance = {3.0f, 2.8f, 2.4f};
    s.waves = nullptr;

    f.DrawV1(s);
    const std::vector<float> a1 = ReadRgba(f.v1.layer().accum_tex);
    const std::vector<float> r1 = ReadRed(f.v1.layer().reveal_tex);
    const std::vector<float> z1 = ReadRed(f.v1.layer().depth_tex);

    f.DrawV2(s, /*sample_count=*/4);
    const std::vector<float> a2 = ReadRgba(f.v2.layer().accum_tex);
    const std::vector<float> r2 = ReadRed(f.v2.layer().reveal_tex);
    const std::vector<float> z2 = ReadRed(f.v2.layer().depth_tex);

    // accum 是 **FP16**（wbo_target.h）⇒ 两个数学等价但指令顺序不同的实现（FMA 合并点不同）
    // 最多差几个 ULP。用「ULP 数」而不是固定相对误差当容差：
    //   0.47 处 1 ULP = 2.4e-4；写 2e-5 实际等于要求浮点位级相同，一次循环重构就会红。
    const auto fp16_ulp = [](float v) {
        const float a = std::fabs(v);
        if (!(a > 0.0f)) {
            return 5.96e-8f;   // 最小次正规数（防御）
        }
        return std::ldexp(1.0f, std::ilogb(a) - 10);
    };

    int covered = 0;
    for (size_t i = 0; i < a1.size(); i += 4) {
        // 只比水面覆盖到的像素（背景 alpha = 0）。
        if (!(a1[i + 3] > 0.0f) && !(a2[i + 3] > 0.0f)) {
            continue;
        }
        ++covered;
        for (int c = 0; c < 4; ++c) {
            // 4 ULP 余量：重新安排循环（例如半向量从循环外移到循环内）就会动 1 ULP。
            // 真正的语义偏差（漏乘 F / 漏除 S / 写错的深度槽）远远大于这个量。
            const float tol = 4.0f * fp16_ulp(a1[i + c]);
            const float d = std::fabs(a2[i + c] - a1[i + c]);
            EXPECT_LE(d, tol) << "accum 分量 " << c << " 与 V1 不一致：V1=" << a1[i + c]
                              << " V2=" << a2[i + c] << "（tol=" << tol << "）";
        }
        EXPECT_LE(std::fabs(r2[i / 4] - r1[i / 4]), 4.0f * fp16_ulp(r1[i / 4]))
            << "reveal 与 V1 不一致";
        EXPECT_LE(std::fabs(z2[i / 4] - z1[i / 4]),
                  2.0e-5f * std::max(1.0f, std::fabs(z1[i / 4])))
            << "深度槽与 V1 不一致";
    }
    EXPECT_GT(covered, kWidth * kHeight / 2) << "覆盖像素太少，用例没有覆盖面";
    // 保证不是「两边都是 0」的假通过：V1 的输出必须真的有非零反射。
    float peak = 0.0f;
    for (size_t i = 0; i < a1.size(); i += 4) {
        peak = std::max(peak, std::fabs(a1[i]));
    }
    EXPECT_GT(peak, 0.0f) << "两个 pass 都输出全 0 —— 先怀疑程序根本没画";
    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}

// ② 有波浪：V2 ≠ V1，且 S=1 ≠ S=8；输出全有限、α ∈ [0,1]。
TEST(WaterSkinV2Gpu, V2WithWavesDiffersAndRespondsToSampleCount) {
    Fixture f;
    f.SetUp();
    const jpov::WaterWaves waves_v1 = MakeTestWaves(/*v2=*/false);
    const jpov::WaterWaves waves_v2 = MakeTestWaves(/*v2=*/true);
    SceneInputs s;
    s.cam.position = {2.0f, 8.0f, -1.0f};
    s.cam.target = {2.0f, 0.0f, 4.0f};   // 俯视约 53°
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.5f;
    s.has_sun = true;
    s.sun_toward = Normalized(Vec3f{0.2f, 0.8f, 0.55f});
    s.sun_irradiance = {2.0f, 1.9f, 1.7f};

    s.waves = &waves_v1;
    f.DrawV1(s);
    const std::vector<float> a1 = ReadRgba(f.v1.layer().accum_tex);

    s.waves = &waves_v2;
    f.DrawV2(s, /*sample_count=*/1);
    const std::vector<float> a_s1 = ReadRgba(f.v2.layer().accum_tex);

    f.DrawV2(s, /*sample_count=*/8);
    const std::vector<float> a_s8 = ReadRgba(f.v2.layer().accum_tex);
    const std::vector<float> r_s8 = ReadRed(f.v2.layer().reveal_tex);

    int finite = 0;
    int alpha_ok = 0;
    int total = 0;
    double sum_v1_vs_s8 = 0.0;
    double sum_s1_vs_s8 = 0.0;
    for (size_t i = 0; i < a1.size(); i += 4) {
        ++total;
        bool all_finite = true;
        for (int c = 0; c < 4; ++c) {
            all_finite = all_finite && std::isfinite(a_s8[i + c]) &&
                         std::isfinite(a_s1[i + c]);
        }
        if (all_finite) {
            ++finite;
        }
        if (all_finite && a_s8[i + 3] >= 0.0f && a_s8[i + 3] <= 1.0f &&
            r_s8[i / 4] >= 0.0f && r_s8[i / 4] <= 1.0f) {
            ++alpha_ok;
        }
        EXPECT_TRUE(std::isfinite(a1[i + 3])) << "V1 也出现非有限值，先怀疑测试输入";
        sum_v1_vs_s8 += std::fabs(a_s8[i + 0] - a1[i + 0]) + std::fabs(a_s8[i + 1] - a1[i + 1]) +
                        std::fabs(a_s8[i + 2] - a1[i + 2]);
        sum_s1_vs_s8 += std::fabs(a_s8[i + 0] - a_s1[i + 0]) +
                        std::fabs(a_s8[i + 1] - a_s1[i + 1]) +
                        std::fabs(a_s8[i + 2] - a_s1[i + 2]);
    }
    EXPECT_EQ(finite, total) << "V2 输出出现 NaN/Inf（相位原点 / 求交 / 平均哪里写歪了）";
    EXPECT_EQ(alpha_ok, total) << "V2 的 α 越界（必须在 [0,1]）";
    const double mean_v1_vs_s8 = sum_v1_vs_s8 / static_cast<double>(total);
    const double mean_s1_vs_s8 = sum_s1_vs_s8 / static_cast<double>(total);
    // 口径差异（不截断 + 多法线）必须明显大于 float 噪声。
    EXPECT_GT(mean_v1_vs_s8, 1.0e-3) << "V2 与 V1 几乎一样 —— 谱截断/多法线其中一条没生效";
    EXPECT_GT(mean_s1_vs_s8, 1.0e-4) << "S=1 与 S=8 几乎一样 —— 多法线采样没接上";
    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}

// ④ 水下支路（平面）：V2 = V1 逐像素。
// 水下是 V2 唯一与 V1 **不等价**的着色路径（逐样本 MediumAt、逐样本 TIR 钳位），
// 但**平面**情况下两者必须完全等价：F 退回平面菲涅尔、镜像方向一致、P 都落在 level−1mm。
// 这条同时覆盖「水下 program 拼装/MediumAt 调用/环外 α=1」三件事。
TEST(WaterSkinV2Gpu, V2FlatUnderwaterMatchesV1) {
    Fixture f;
    f.SetUp();
    SceneInputs s;
    s.cam.position = {0.0f, -3.0f, 0.0f};   // 水下 3 m
    // 朝上 63° 俯仰看：Snell 窗半角 48.6°（临界角）⇒ 画面下半是环外（TIR）、上半是窗内。
    // 正上方看会整屏落在窗内，太平时整屏 TIR —— 两种都覆盖不到另一支。
    s.cam.target = {0.0f, 3.0f, -3.0f};
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.0f;
    s.has_sun = true;
    s.sun_toward = Normalized(Vec3f{0.5f, 0.7071f, 0.5f});
    s.sun_irradiance = {2.0f, 1.9f, 1.7f};
    s.waves = nullptr;

    jpov::water::WaterMediumSample medium;
    medium.level_y = s.level_y;
    medium.ior = s.ior;
    medium.sigma_a = {0.35f, 0.054f, 0.014f};
    medium.sigma_s = {0.05f, 0.05f, 0.05f};
    medium.phase_g = 0.35f;
    medium.ambient = {0.45f, 0.55f, 0.70f};
    medium.sun_color = {2.0f, 1.9f, 1.7f};
    medium.sun_toward = {0.5f, 0.7071f, 0.5f};
    medium.has_sun = true;
    // ToInputs 里 medium 恒为 nullptr（水上用例）；这里补上（水下必须给介质）。
    const auto draw_v1 = [&]() {
        WaterSkinRenderer::Inputs in = ToInputs(s, f.cube);
        in.medium = &medium;
        f.v1.Begin(kWidth, kHeight);
        f.v1.Draw(in);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    const auto draw_v2 = [&]() {
        WaterSkinV2Renderer::Inputs in = ToInputs(s, f.cube);
        in.medium = &medium;
        f.v2.Begin(kWidth, kHeight);
        f.v2.Draw(in, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };

    draw_v1();
    const std::vector<float> a1 = ReadRgba(f.v1.layer().accum_tex);
    const std::vector<float> r1 = ReadRed(f.v1.layer().reveal_tex);
    const std::vector<float> z1 = ReadRed(f.v1.layer().depth_tex);

    draw_v2();
    const std::vector<float> a2 = ReadRgba(f.v2.layer().accum_tex);
    const std::vector<float> r2 = ReadRed(f.v2.layer().reveal_tex);
    const std::vector<float> z2 = ReadRed(f.v2.layer().depth_tex);

    int covered = 0;
    int tir = 0;      // 环外（α = 1）
    int window = 0;   // 窗内（α 很小）
    for (size_t i = 0; i < a1.size(); i += 4) {
        if (!(a1[i + 3] > 0.0f) && !(a2[i + 3] > 0.0f)) {
            continue;
        }
        ++covered;
        if (a1[i + 3] > 0.999f) {
            ++tir;
        }
        if (a1[i + 3] < 0.1f) {
            ++window;
        }
        for (int c = 0; c < 4; ++c) {
            const float scale = std::max(1.0f, std::fabs(a1[i + c]));
            const float d = std::fabs(a2[i + c] - a1[i + c]);
            EXPECT_LE(d / scale, 2.0e-5f)
                << "水下 accum 分量 " << c << " 与 V1 不一致：V1=" << a1[i + c]
                << " V2=" << a2[i + c];
        }
        EXPECT_LE(std::fabs(r2[i / 4] - r1[i / 4]), 2.0e-5f) << "水下 reveal 与 V1 不一致";
        EXPECT_LE(std::fabs(z2[i / 4] - z1[i / 4]),
                  2.0e-5f * std::max(1.0f, std::fabs(z1[i / 4])))
            << "水下深度槽与 V1 不一致";
    }
    EXPECT_GT(covered, kWidth * kHeight / 2) << "覆盖像素太少，用例没有覆盖面";
    // 保证两条分支都真的出现过（否则这条用例只测了其中一支）。
    EXPECT_GT(tir, 20) << "没有环外（α=1）像素：机位选错了，TIR 分支没覆盖";
    EXPECT_GT(window, 20) << "没有窗内（α<0.1）像素：Snell 窗分支没覆盖";
    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}

// ③ 采样数越小越「噪」：S=1 的像素间方差必须大于 S=16。
//    这是 docs §2.1 那张「噪声起点」表的可执行版本 —— 若 S 不影响空间方差，
//    多法线平均就只是「同样一次着色算了很多遍」。
TEST(WaterSkinV2Gpu, LargerSampleCountReducesPixelToPixelVariance) {
    Fixture f;
    f.SetUp();
    const jpov::WaterWaves waves_v2 = MakeTestWaves(/*v2=*/true);
    SceneInputs s;
    s.cam.position = {0.0f, 3.0f, -30.0f};
    s.cam.target = {0.0f, 0.0f, -200.0f};   // 掠视远处（足迹大 ⇒ 采样最容易不够）
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.0f;
    s.waves = &waves_v2;

    const auto lateral_variance = [&](int samples) {
        f.DrawV2(s, samples);
        const std::vector<float> a = ReadRgba(f.v2.layer().accum_tex);
        // 相邻像素差（横向）的平均绝对值：水面纹理越碎 ⇒ 越大。
        double sum = 0.0;
        int n = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x + 1 < kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                sum += std::fabs(a[(i + 1) * 4 + 0] - a[i * 4 + 0]);
                ++n;
            }
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    };
    const double v1_sample = lateral_variance(1);
    const double v16_sample = lateral_variance(16);
    SCOPED_TRACE(testing::Message() << "S=1 邻差=" << v1_sample << " S=16 邻差=" << v16_sample);
    EXPECT_GT(v1_sample, 0.0) << "S=1 时画面完全平（可能整片是同一法线）";
    // 留余量（不写裸 <）：这条是统计性判据，比值够大才算「真的被平均掉了」
    //（S=16 的理论降噪是 1/√16 = 0.25；取 0.7 当门限，只要求"明显变小"）。
    EXPECT_LT(v16_sample, 0.7 * v1_sample) << "S=16 没有明显比 S=1 更平滑";
    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}

// ⑥ 谱截断旋钮 T：乘数必须真的接在阈值上。
//    判据用**法线可视化**（debug=1）而不是颜色：截断只影响几何，颜色还混着 F / 探针，
//    在法线上测才能把“截断生效”与“着色变了”分开。
//    掠视远处 ⇒ 细节波是亚像素 ⇒ T=2（6~16 px）必须把它杀掉，横向邻差明显变小。
TEST(WaterSkinV2Gpu, V2TruncationKnobKillsSubPixelWaves) {
    Fixture f;
    f.SetUp();
    jpov::WaterWaves waves = MakeTestWaves(/*v2=*/true);
    SceneInputs s;
    s.cam.position = {0.0f, 3.0f, -30.0f};
    s.cam.target = {0.0f, 0.0f, -200.0f};   // 掠视远处（足迹大）
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.0f;
    s.wave_debug = 1;   // 法线可视化（N*0.5+0.5）

    const auto lateral_delta = [&](float truncate_px) {
        jpov::WaterWaves w = waves;
        // 这条用例只关心「截断有没有生效」，两个族给同一个 T。
        w.v2_truncate_px_main = truncate_px;
        w.v2_truncate_px_detail = truncate_px;
        s.waves = &w;
        f.DrawV2(s, 4);
        const std::vector<float> a = ReadRgba(f.v2.layer().accum_tex);
        double sum = 0.0;
        int n = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x + 1 < kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                sum += std::fabs(a[(i + 1) * 4 + 0] - a[i * 4 + 0]);
                ++n;
            }
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    };
    const double d_off = lateral_delta(0.0f);   // 不截断
    const double d_t2 = lateral_delta(2.0f);    // 6~16 px
    SCOPED_TRACE(testing::Message() << "T=0 邻差=" << d_off << " T=2 邻差=" << d_t2);
    EXPECT_GT(d_off, 0.0) << "T=0 时法线完全平 —— 测试机位没看到波";
    EXPECT_LT(d_t2, 0.7 * d_off) << "T=2 没有把亚像素波杀掉 —— 旋钮没接到阈值上";

    // T=1（同一对阈值的乘 1）必须与“不截断”不同 —— 否则区别不了 0 与 1 的语义。
    const double d_t1 = lateral_delta(1.0f);
    EXPECT_LT(d_t1, d_off) << "T=1 与不截断完全一样（远处本应已经截断）";
    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}


// ⑦ 两个截断旋钮**互相独立**（Danis 2026-11-26：主波 T / 细节波 T 分开）。
//
// 判据不用「删掉一族波」（那样 wave uploader 会因为 num_waves = 0 整体关掉，测出来是假通过），
// 而是利用**两族波长差一个数量级**：
//   镜头贴在 0.55 m 高、近乎垂直俯视，取 T = 10 ⇒ 阈值 30~80 px：
//     主波最短 3 m ≈ 131 px ⇒ **仍在线上**（余量 1.6×），主波 T 不该有任何影响；
//     细节波 0.5 m ≈ 21 px ⇒ **被线扫到**，细节波 T 必须有效。
//   ⚠️ 两个坑（都踩过）：① T 取太大（如 20）会把 3 m 主波也扫进去，判据失效；
//      ② 本用例的视口只有 48×32 ⇒ `pixel_angle ≈ 0.036 rad`，是 1280×720 的 **20 倍**，
//         所以“近场”得把相机摆到 0.5 m 高，否则 footprint 已经是 0.25 m（3 m 波只有 12 px）。
// 反过来，掠射机位下主波 T 必须有效（细节波在这里也全在线上，不影响结论）。
TEST(WaterSkinV2Gpu, V2TruncationKnobsAreIndependent) {
    Fixture f;
    f.SetUp();
    const jpov::WaterWaves waves = MakeTestWaves(/*v2=*/true);
    SceneInputs s;
    s.depth_tex = MakeDepthTexture();
    s.level_y = 0.0f;
    s.wave_debug = 1;   // 法线可视化：只看几何
    s.waves = &waves;

    const auto render = [&](float main_t, float detail_t) {
        jpov::WaterWaves w = waves;
        w.v2_truncate_px_main = main_t;
        w.v2_truncate_px_detail = detail_t;
        s.waves = &w;
        f.DrawV2(s, 4);
        return ReadRgba(f.v2.layer().accum_tex);
    };
    const auto mean_abs_diff = [](const std::vector<float>& a, const std::vector<float>& b) {
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 0; i + 3 < a.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                sum += std::fabs(a[i + c] - b[i + c]);
            }
            n += 3;
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    };

    // ── 近场：几乎垂直俯视（视场内 EVERY 像素的 footprint 都小），这样主波不可能被扫到 ──
    // ⚠️ 别用 30° 俯视：取景里包含远处几行，那里主波照样被截断 ⇒ 测出来的“主波 T 有影响”
    //    是真的但与本判据无关（这是第一版写成 0.09 的原因）。
    s.cam.position = {0.01f, 0.55f, 0.01f};
    s.cam.target = {0.0f, 0.0f, 0.0f};
    s.cam.fov = 60.0f;
    s.cam.near = kNear;
    s.cam.far = kFar;

    const std::vector<float> n_base = render(0.0f, 0.0f);
    const std::vector<float> n_main = render(10.0f, 0.0f);
    const std::vector<float> n_detail = render(0.0f, 10.0f);
    // 主波几乎不该动：只要求「≤ FP16 噪声量级」。不写 0 是因为视场角落的 footprint
    // 仍有 ~2× 余量差，3 m 主波会在那里被轻轻削一点（实测 1.6e-5，而细节波那边是 1e-2 量级）。
    EXPECT_LT(mean_abs_diff(n_base, n_main), 1.0e-4)
        << "近场时主波全在截断线上方，主波 T=10 却明显改变了结果 —— 两支没分开";
    EXPECT_GT(mean_abs_diff(n_base, n_detail), 1.0e-3)  // T=10：细节波必须被扫掉
        << "近场时细节波 T=10 没有任何影响 —— 细节波那一支没接上";

    // ── 掠射：主波 T 必须有效（这条同时验证「主波支路仍活着」）──
    s.cam.position = {0.0f, 3.0f, -30.0f};
    s.cam.target = {0.0f, 0.0f, -200.0f};
    const std::vector<float> g_base = render(0.0f, 0.0f);
    const std::vector<float> g_main = render(1.0f, 0.0f);
    EXPECT_GT(mean_abs_diff(g_base, g_main), 1.0e-4)
        << "掠射时主波 T=1 没有任何影响 —— 主波那一支没接上";

    glDeleteTextures(1, &s.depth_tex);
    f.TearDown();
}

}  // namespace

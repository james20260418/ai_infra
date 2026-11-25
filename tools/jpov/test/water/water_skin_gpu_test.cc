// JPOV 水皮 GPU 像素测试 —— 锁住「CPU 单测验不到」的那半边
//
// 三板斧：
//   ① **逐像素对拍**：用 CPU 参考模型（water_skin_math.h）+ 已知的 env cubemap /
//      场景深度 / 背景色，算出期望颜色，与真实渲染结果逐像素比较。着色器里任何
//      一处与模型走样（ray 反构、菲涅尔、WBO 的 α/reveal 语义、深度拒绝）都会红。
//   ② WBO 层的三个附件语义：att0/att1/att3 的数值必须满足
//        accum = 反射色×F、reveal = 1−F（FS 写 α，混合后）、depth = z_view×F。
//      —— 「reveal 槽写 1−α 还是 α」正是 2026-11-15 首帧实际踩过的错（画面变成
//      反相菲涅尔：掠射变暗、俯视变亮）。
//   ③ 深度拒绝：场景比水面更近 ⇒ 该像素**一个字节都不该被水皮改**（走 WBO 的
//      accum.a<=1e-7 早退）。
//
// 合成输入，不需要场景 / 物体 / 相机矩阵：水皮的输入本来就只有「env + 场景深度 + 相机」。
//
// 负向验证（提交前必须真的跑一遍这几条，证明断言有牙）：
//   · 把 shader 的 oReveal 改回 1−F ⇒ ① 红（颜色整体反相）
//   · 把深度拒绝的条件反过来 ⇒ ③ 红
//   · 把菲涅尔换成 F0 常量（去掉角度依赖）⇒ ① 红（各像素色差不再是 2%…90%）

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
#include "tools/jpov/src/water/water_skin_math.h"
#include "tools/jpov/src/water/water_medium_math.h"
#include "tools/jpov/src/water/water_skin_renderer.h"

namespace {

using jpov::Camera;
using jpov::Vec3f;
using jpov::WaterSkinRenderer;
using jpov::water::EvaluateWaterPixel;
using jpov::water::WaterPixel;

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
        cfg.title = "Water Skin GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// ───────────────────────── 测试资源 ─────────────────────────

// 天光探针：所有面同一个「天空色」，只有下半球 (−Y) 是黑（＝地色）。
// 上半球全同色是刻意的：反射方向贴着面边界时不会因为「选了哪个面」而对拍失败，
// 同时仍能抓住「采样到了下半球地色」这类真实错误。
constexpr float kSkyR = 0.50f;
constexpr float kSkyG = 0.60f;
constexpr float kSkyB = 1.00f;
constexpr float kEnvScale = 1.0f;
constexpr float kTirR = 0.02f;
constexpr float kTirG = 0.05f;
constexpr float kTirB = 0.065f;

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

unsigned int MakeDepthTexture(const std::vector<float>& values) {
    CHECK_EQ(static_cast<int>(values.size()), kWidth * kHeight);
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

// ───────────────────────── CPU 参考（与着色器逐行对应）─────────────────────────

struct Reference {
    // 与 Primitives3DRenderer::BuildMVP / WaterSkinRenderer 同一套基。
    Vec3f fwd;
    Vec3f right;
    Vec3f up;
    float tan_half_fov = 0.0f;
    float aspect = 1.0f;
};

Vec3f NormalizeOr(const Vec3f& v, const Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1.0e-8f) {
        return fallback;
    }
    return Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

Reference BuildReference(const Camera& cam) {
    Reference r;
    // 与 WaterSkinRenderer::UploadCommonUniforms / Primitives3DRenderer::BuildMVP 同一套
    // 退化回退（相机正上/正下看时 up ∥ fwd，cross 为 0）。
    r.fwd = NormalizeOr(Vec3f(cam.target.x() - cam.position.x(),
                              cam.target.y() - cam.position.y(),
                              cam.target.z() - cam.position.z()),
                        Vec3f(0.0f, 0.0f, -1.0f));
    const Vec3f side = Vec3f(r.fwd.y() * cam.up.z() - r.fwd.z() * cam.up.y(),
                             r.fwd.z() * cam.up.x() - r.fwd.x() * cam.up.z(),
                             r.fwd.x() * cam.up.y() - r.fwd.y() * cam.up.x());
    r.right = NormalizeOr(side, Vec3f(1.0f, 0.0f, 0.0f));
    r.up = Vec3f(r.right.y() * r.fwd.z() - r.right.z() * r.fwd.y(),
                 r.right.z() * r.fwd.x() - r.right.x() * r.fwd.z(),
                 r.right.x() * r.fwd.y() - r.right.y() * r.fwd.x());
    r.tan_half_fov = std::tan(0.5f * cam.fov * 3.14159265358979323846f / 180.0f);
    r.aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    return r;
}

// 与着色器同一条 ray 反构；x/y 是**像素中心**（y 从下往上，与 gl_FragCoord 一致）。
Vec3f RayAt(const Reference& ref, int x, int y) {
    const float ndc_x = (static_cast<float>(x) + 0.5f) / kWidth * 2.0f - 1.0f;
    const float ndc_y = (static_cast<float>(y) + 0.5f) / kHeight * 2.0f - 1.0f;
    return NormalizeOr(Vec3f(ref.fwd.x() + ref.right.x() * (ndc_x * ref.tan_half_fov * ref.aspect)
                                 + ref.up.x() * (ndc_y * ref.tan_half_fov),
                             ref.fwd.y() + ref.right.y() * (ndc_x * ref.tan_half_fov * ref.aspect)
                                 + ref.up.y() * (ndc_y * ref.tan_half_fov),
                             ref.fwd.z() + ref.right.z() * (ndc_x * ref.tan_half_fov * ref.aspect)
                                 + ref.up.z() * (ndc_y * ref.tan_half_fov)),
                      ref.fwd);
}


}  // namespace

// ───────────────────────── CPU 参考：层语义（C / α / 射线距离）─────────────────────────
//
// 与 water_skin_shader.h 逐行对应；水下支路的 S_P(∞) 直接用 water_medium_math.h 的
// WaterAt（z = 1e6），保证「同一个积分器取极限」，不是另写一份公式。
struct LayerExpect {
    bool hit = false;
    Vec3f color = {0.0f, 0.0f, 0.0f};   // 层色（预乘）
    float alpha = 0.0f;                 // 遮挡率
    float ray_t = 0.0f;                 // 射线距离
};

LayerExpect ExpectLayer(const Camera& cam, const Reference& ref,
                        const std::vector<float>& depth, int x, int y,
                        float level_y, float ior,
                        const jpov::water::WaterMediumSample& medium,
                        const jpov::WaterWaves* waves = nullptr,
                        float flow_x = 0.0f, float flow_z = 0.0f,
                        float time_s = 0.0f, float depth_m = 300.0f) {
    // 与着色器同口径：每像素弧度（GPU 侧是 2*uTanHalfFov/uViewport.y）。
    const float pixel_angle_rad = 2.0f * ref.tan_half_fov / static_cast<float>(kHeight);
    LayerExpect e;
    const Vec3f rd = RayAt(ref, x, y);
    const size_t i = static_cast<size_t>(y) * kWidth + x;
    WaterPixel px;
    if (!EvaluateWaterPixel(cam.position, ref.fwd, rd, level_y, ior, depth[i],
                            cam.near, cam.far, &px, waves, flow_x, flow_z,
                            pixel_angle_rad, time_s, depth_m)) {
        return e;
    }
    e.hit = true;
    const float dot_fwd = rd.Dot(ref.fwd);
    const float t = px.view_z / dot_fwd;   // 视轴深度 → 射线距离
    e.ray_t = t;

    const bool under = cam.position.y() < level_y;
    if (under) {
        // 与着色器同一条约定：P 往水面下推 1 mm，避免落在平面上时分支不确定。
        const Vec3f P{cam.position.x() + rd.x() * t,
                      cam.position.y() + rd.y() * t - 1.0e-3f,
                      cam.position.z() + rd.z() * t};
        const Vec3f mirror{rd.x(), -rd.y(), rd.z()};
        Vec3f tau;
        Vec3f S;
        jpov::water::WaterAt(medium, P, mirror, 1.0e6f, &tau, &S);
        e.color = Vec3f{S.x() * px.reflectance, S.y() * px.reflectance,
                        S.z() * px.reflectance};
        e.alpha = px.reflectance;                 // 环外 Fresnel = 1 ⇒ α = 1
    } else {
        const Vec3f rdir = jpov::water::ClampReflectionAboveHorizon(
            jpov::water::ReflectDirectionAbout(rd, px.normal));
        e.color = Vec3f{kSkyR * kEnvScale * px.reflectance,
                        kSkyG * kEnvScale * px.reflectance,
                        kSkyB * kEnvScale * px.reflectance};
        // 透射率 = (1−F)/n² ⇒ WBO 的 α（背景衰减率）= 1 − (1−F)/n²
        e.alpha = 1.0f - (1.0f - px.reflectance) / (ior * ior);
        (void)rdir;
    }
    return e;
}

// ───────────────────────── 测试主体 ─────────────────────────

struct Fixture {
    GlEnv& env = GlEnv::Get();
    jpov::ShaderManager shader_mgr;
    WaterSkinRenderer water;
    unsigned int cube = 0;

    void SetUp() {
        env.Ensure();
        cube = MakeEnvCube();
        water.Init(&shader_mgr);
    }
    void TearDown() {
        water.Finalize();
        if (cube != 0) {
            glDeleteTextures(1, &cube);
            cube = 0;
        }
    }

    void Render(const Camera& cam, unsigned int depth_tex, float level_y, float ior,
                const jpov::water::WaterMediumSample* medium,
                bool has_sun = false,
                jpov::Vec3f sun_toward = jpov::Vec3f{0.0f, 1.0f, 0.0f},
                jpov::Vec3f sun_irradiance = jpov::Vec3f{0.0f, 0.0f, 0.0f},
                float sun_angular_radius = jpov::lighting::kSunAngularRadiusRad,
                float sun_footprint = 0.0f,
                const jpov::WaterWaves* waves = nullptr,
                unsigned int wave_flow_tex = 0, float wave_time_s = 0.0f,
                float wave_depth_m = 300.0f, int wave_debug = 0) {
        WaterSkinRenderer::Inputs in;
        in.camera = cam;
        in.full_w = kWidth;
        in.full_h = kHeight;
        in.scene_depth_tex = depth_tex;
        in.env_cube_tex = cube;
        in.env_scale = kEnvScale;
        in.level_y = level_y;
        in.ior = ior;
        in.medium = medium;
        in.has_sun = has_sun;
        in.sun_toward = sun_toward;
        in.sun_irradiance = sun_irradiance;
        in.sun_angular_radius = sun_angular_radius;
        // 测试里默认关足迹（着色器的 dFdx 无法在 CPU 复现）；专门验足迹的用例再开。
        in.sun_footprint = sun_footprint;
        in.waves = waves;
        in.wave_flow_tex = wave_flow_tex;
        in.wave_time_s = wave_time_s;
        in.wave_depth_m = wave_depth_m;
        in.wave_debug = wave_debug;   // 1 = 输出法线色（N*0.5+0.5），供回归用例直接读回
        water.Begin(kWidth, kHeight);
        water.Draw(in);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

jpov::water::WaterMediumSample MakeMedium(float level_y, float ior) {
    jpov::water::WaterMediumSample m;
    m.level_y = level_y;
    m.ior = ior;
    m.sigma_a = {0.35f, 0.054f, 0.014f};
    m.sigma_s = {0.05f, 0.05f, 0.05f};
    m.phase_g = 0.35f;
    m.ambient = {0.45f, 0.55f, 0.70f};
    m.sun_color = {2.0f, 1.9f, 1.7f};
    m.sun_toward = {0.5f, 0.7071f, 0.5f};
    m.has_sun = true;
    return m;
}

// 水下：环外（超临界）α 必须是 1，颜色必须是 S_P(∞)（同一个 MediumAt 取 z→∞）。
TEST(WaterSkinGpu, UnderwaterRingIsOpaqueDeepSea) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, -3.0f, 0.0f};   // 水下 3 m
    cam.target = {0.0f, 0.0f, -20.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const jpov::water::WaterMediumSample medium = MakeMedium(level, ior);

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, &medium);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior, medium);
            if (!e.hit) {
                continue;
            }
            // 只比环外：Fresnel 把 F 顶到 1 的像素（留 1e-3 余量避开临界角边界）。
            if (e.alpha < 0.999f) {
                continue;
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            EXPECT_NEAR(accum[base * 4 + 0], e.color.x(), 2.0e-3f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(accum[base * 4 + 1], e.color.y(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.color.z(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], 1.0f, 1.0e-3f);
            EXPECT_NEAR(reveal[base], 0.0f, 1.0e-3f);
            ++compared;
        }
    }
    EXPECT_GT(compared, 20) << "环外像素太少，用例没有覆盖面";
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 水下：环内（窗）α = F（几 %），颜色 = F·S_P(∞)，且深度槽存的是 **射线距离 × α**。
TEST(WaterSkinGpu, UnderwaterWindowIsFresnelFilmAndDepthIsRayDistance) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, -3.0f, 0.0f};
    cam.target = {0.0f, 0.0f, 0.0f};   // 正上方看：整屏落在 Snell 窗内（否则全是 TIR）
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const jpov::water::WaterMediumSample medium = MakeMedium(level, ior);

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, &medium);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const std::vector<float> zsum = ReadRed(f.water.layer().depth_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior, medium);
            if (!e.hit || e.alpha > 0.5f) {
                continue;   // 只看窗内（F 很小）
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            EXPECT_NEAR(accum[base * 4 + 0], e.color.x(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 1], e.color.y(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.color.z(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], e.alpha, 1.0e-3f);
            EXPECT_NEAR(reveal[base], 1.0f - e.alpha, 1.0e-3f);
            // ⚠️ 深度槽必须是「射线距离 × α」，不是视轴深度 —— 这是 2026-11-16 定稿的口径。
            EXPECT_NEAR(zsum[base], e.ray_t * e.alpha, 1.0e-2f);
            ++compared;
        }
    }
    EXPECT_GT(compared, 60) << "窗内像素太少，用例没有覆盖面";
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 水上：α 必须是 1 − (1−F)/n²（÷n² 折进透射率），颜色 = F·sky(mirror)。
TEST(WaterSkinGpu, AboveWaterTransmissionCarriesDivNSquared) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const jpov::water::WaterMediumSample medium = MakeMedium(level, ior);

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, &medium);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior, medium);
            if (!e.hit) {
                continue;
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            EXPECT_NEAR(accum[base * 4 + 0], e.color.x(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 1], e.color.y(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.color.z(), 2.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], e.alpha, 1.0e-3f);
            EXPECT_NEAR(reveal[base], 1.0f - e.alpha, 1.0e-3f);
            ++compared;
        }
    }
    EXPECT_GT(compared, 100) << "水上像素太少，用例没有覆盖面";
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 深度拒绝：不透明场景挡在水面前 ⇒ 该像素一个 fragment 都不许写（α = 0、reveal = 1）。
TEST(WaterSkinGpu, DepthRejectionLeavesLayerEmpty) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;

    // 深度图写 0.0 ⇒ 场景就贴在近裁剪面上（任何水面交点都比它远）。
    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 0.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    const jpov::water::WaterMediumSample medium = MakeMedium(level, 1.333f);
    f.Render(cam, depth_tex, level, 1.333f, &medium);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const size_t px_count = static_cast<size_t>(kWidth) * kHeight;
    for (size_t i = 0; i < px_count; ++i) {
        EXPECT_NEAR(reveal[i], 1.0f, 1.0e-3f) << "i=" << i;
        EXPECT_NEAR(accum[i * 4 + 3], 0.0f, 1.0e-3f) << "i=" << i;
    }
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 水上：太阳的像（解析 GGX，盘状光源）必须逐像素等于 CPU 参考。
//
// 口径（docs/jpov_specular_bandwidth.md §1.6）：
//   · 带宽 = (θ/2)²（半向量只偏一半），不是 θ²；
//   · 出射 = E·F·D·G/(4 NoV NoL)·NoL，F 用**同一个**界面菲涅尔（与探针支路同源）；
//   · 太阳项是**加法**，不吃任何 SSR 权重（此刻还没有 SSR）。
// footprint 置 0：着色器里的 dFdx 在 CPU 复现不了；足迹由下一条用例定性验证。
TEST(WaterSkinGpu, AboveWaterSunDiskMatchesCpu) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const float theta = jpov::lighting::kSunAngularRadiusRad;
    const Vec3f sun_e{3.0f, 2.8f, 2.4f};

    // 把太阳放在**屏幕中心像素**的反射方向上：保证至少有一个像素落在 lobe 内。
    const Reference ref0 = BuildReference(cam);
    const Vec3f rd_center = RayAt(ref0, kWidth / 2, kHeight / 2);
    const Vec3f sun_toward = NormalizeOr(Vec3f{rd_center.x(), -rd_center.y(), rd_center.z()},
                                        Vec3f{0.0f, 1.0f, 0.0f});
    ASSERT_GT(sun_toward.y(), 0.0f) << "太阳必须在水平面以上，否则这一 frame 没有日盘像";

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr,
             /*has_sun=*/true, sun_toward, sun_e, theta, /*sun_footprint=*/0.0f);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const Reference ref = BuildReference(cam);
    const Vec3f normal{0.0f, 1.0f, 0.0f};

    int compared = 0;
    float peak = 0.0f;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            const Vec3f rd = RayAt(ref, x, y);
            WaterPixel px;
            // 无介质：水上支路的层色只取决于 F 与探针（+ 太阳项）。
            if (!EvaluateWaterPixel(cam.position, ref.fwd, rd, level, ior, 1.0f,
                                    cam.near, cam.far, &px)) {
                continue;
            }
            const Vec3f toward_cam{-rd.x(), -rd.y(), -rd.z()};
            const Vec3f sun = jpov::lighting::SunDiskSpecularRadiance(
                normal, toward_cam, sun_toward, px.reflectance, theta,
                /*extra_a2=*/0.0f, sun_e);
            const float exp_r = kSkyR * kEnvScale * px.reflectance + sun.x();
            const float exp_g = kSkyG * kEnvScale * px.reflectance + sun.y();
            const float exp_b = kSkyB * kEnvScale * px.reflectance + sun.z();
            // 相对容差：太阳峰值是 1e3 量级，绝对容差在这里没意义；float 在 NoH→1
            // 处有 ~0.3% 抵消误差（见 ggx_specular.h）。
            const float scale = std::max(1.0f, exp_r);
            EXPECT_NEAR(accum[base * 4 + 0], exp_r, scale * 1.0e-2f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(accum[base * 4 + 1], exp_g, scale * 1.0e-2f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(accum[base * 4 + 2], exp_b, scale * 1.0e-2f) << "x=" << x << " y=" << y;
            peak = std::max(peak, accum[base * 4 + 0]);
            ++compared;
        }
    }
    EXPECT_GT(compared, 500) << "水上像素太少，用例没有覆盖面";
    // 日盘像必须**真的出现**（否则「全 0 也全对」，这条断言就是空测）。
    // 峰值 ≈ E·F·D(1)/4 = 3·0.02·(1/(π·5.41e-6))/4 ≈ 880。
    EXPECT_GT(peak, 100.0f) << "没有出现日盘像：>0 说明带宽/遮罩哪一步被关掉了";
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 像素足迹（specular AA）：开启后 lobe 必须变宽（被点亮像素数增加）且峰值下降。
// 这是「掠射时投影把像压回亚像素」的正解 —— 关掉它就该退化成稀疏亮点。
TEST(WaterSkinGpu, SunFootprintWidensTheLobe) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const float theta = jpov::lighting::kSunAngularRadiusRad;
    const Vec3f sun_e{3.0f, 2.8f, 2.4f};

    const Reference ref0 = BuildReference(cam);
    const Vec3f rd_center = RayAt(ref0, kWidth / 2, kHeight / 2);
    const Vec3f sun_toward = NormalizeOr(Vec3f{rd_center.x(), -rd_center.y(), rd_center.z()},
                                        Vec3f{0.0f, 1.0f, 0.0f});

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);

    struct Stats {
        int lit = 0;
        float peak = 0.0f;
    };
    const auto measure = [&](float footprint) {
        f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr,
                 /*has_sun=*/true, sun_toward, sun_e, theta, footprint);
        const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
        Stats st;
        for (int i = 0; i < kWidth * kHeight; ++i) {
            const float r = accum[i * 4 + 0];
            if (r > 2.0f) {   // 只数「明显比环境反射亮」的像素
                ++st.lit;
            }
            st.peak = std::max(st.peak, r);
        }
        return st;
    };

    const Stats off = measure(0.0f);
    const Stats on = measure(1.0f);
    // 48×32 的小视口里一个像素 ≈ 1.9°，「盘」只有 0.53° ⇒ 关足迹时几乎只能碰到 1~2 像素。
    EXPECT_GT(off.lit, 0) << "关足迹时连一个像素都没点亮，先怀疑日盘像根本没出现";
    EXPECT_GT(on.lit, off.lit) << "足迹没把 lobe 变宽：dFdx 那条路没接上";
    EXPECT_LT(on.peak, off.peak) << "变宽必须伴随峰值下降（能量守恒），否则是在凭空加能量";

    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// ───────────────────────── 波浪（高度置换 + 扰动法线）─────────────────────────

jpov::WaterWaves MakeTestWaves() {
    jpov::WaterWaves w;
    w.enabled = true;
    w.num_waves = 3;
    w.waves[0] = jpov::WaterWave{1.0f, 0.0f, 9.0f, 0.35f, 0.2f};
    w.waves[1] = jpov::WaterWave{0.4f, 0.9f, 5.0f, 0.18f, 1.4f};
    w.waves[2] = jpov::WaterWave{-0.7f, 0.7f, 3.0f, 0.08f, 2.2f};
    return w;
}

// 无 flow：GPU 必须与 CPU 参考逐像素一致（求交 / 法线 / 菲涅尔 / 层语义）。
// 同时证明「位移真的把交点推动了」——否则这个用例只验证了平面。
TEST(WaterSkinGpu, WavesMatchCpuReference) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {2.0f, 8.0f, -1.0f};
    cam.target = {2.0f, 0.0f, 4.0f};   // 俯视约 53°
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.5f;
    const float ior = 1.333f;
    const jpov::WaterWaves waves = MakeTestWaves();

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/0, /*wave_time_s=*/1.3f);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const std::vector<float> zsum = ReadRed(f.water.layer().depth_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    int displaced = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior,
                                              jpov::water::WaterMediumSample{}, &waves,
                                              0.0f, 0.0f, /*time_s=*/1.3f);
            if (!e.hit) {
                continue;
            }
            // 平面解作对照：交点被推动 > 2 cm 才计入 displaced。
            const LayerExpect flat = ExpectLayer(cam, ref, depth, x, y, level, ior,
                                                 jpov::water::WaterMediumSample{});
            if (flat.hit && std::fabs(flat.ray_t - e.ray_t) > 0.02f) {
                ++displaced;
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            EXPECT_NEAR(accum[base * 4 + 0], e.color.x(), 4.0e-3f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(accum[base * 4 + 1], e.color.y(), 4.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.color.z(), 4.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], e.alpha, 3.0e-3f);
            EXPECT_NEAR(reveal[base], 1.0f - e.alpha, 3.0e-3f);
            // att3 = 射线距离 × α：波浪交点必须与 CPU 的 Newton 解一致。
            EXPECT_NEAR(zsum[base], e.ray_t * e.alpha, 0.02f + 0.01f * std::fabs(e.ray_t));
            ++compared;
        }
    }
    EXPECT_GT(compared, 20) << "覆盖像素太少";
    EXPECT_GT(displaced, 5) << "波浪没有真的推动交点（求交可能退回了平面解）";

    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 细节短波 + 有限水深：GPU 与 CPU 参考必须用**同一个 ω**。
// 锁两件事（都是 2026-11-21 P8 抓到的）：
//   ① 细节波的色散必须走 DispersionOmega(k, depth)（GPU 曾写死深水 sqrt(9.81k)）；
//   ② 求交/二分与最终采样必须同一个 depth（CPU 二分曾漏传 depth）。
// 敏感性自我证明：同一份 GPU 结果与「深水 CPU 参考」必须明显不一致（负向对照），
// 否则这条用例只证明了"两遍都用深水"。
TEST(WaterSkinGpu, DetailWavesAndShallowDepthMatchCpu) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {2.0f, 8.0f, -1.0f};
    cam.target = {2.0f, 0.0f, 4.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.5f;
    const float ior = 1.333f;
    const float depth_m = 1.0f;    // 浅水：λ=9 m 主波的 ω 只有深水的 0.78 倍
    const float time_s = 2.5f;

    jpov::WaterWaves waves = MakeTestWaves();
    const float kTwoPi = 6.28318530717958647692f;
    const float kDetailLam[4] = {8.0f, 6.0f, 4.0f, 2.5f};
    waves.num_detail_waves = 4;
    for (int i = 0; i < 4; ++i) {
        jpov::WaterWave d;
        const float ang = 0.7f * static_cast<float>(i);
        d.dir_x = std::sin(ang);
        d.dir_z = std::cos(ang);
        d.wavelength_m = kDetailLam[i];
        d.amplitude_m = 0.07f * kDetailLam[i] / kTwoPi;   // 与 BuildWater 同口径的陡度
        d.phase_rad = 1.1f * static_cast<float>(i);
        waves.detail_waves[i] = d;
    }

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/0, /*wave_time_s=*/time_s, /*wave_depth_m=*/depth_m);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> zsum = ReadRed(f.water.layer().depth_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    float worst_shallow = 0.0f;    // GPU vs 浅水 CPU（必须很小）
    float worst_deep = 0.0f;       // GPU vs 深水 CPU（必须明显，证明用例有感）
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect shallow =
                ExpectLayer(cam, ref, depth, x, y, level, ior,
                            jpov::water::WaterMediumSample{}, &waves, 0.0f, 0.0f,
                            time_s, depth_m);
            if (!shallow.hit) {
                continue;
            }
            const LayerExpect deep =
                ExpectLayer(cam, ref, depth, x, y, level, ior,
                            jpov::water::WaterMediumSample{}, &waves, 0.0f, 0.0f,
                            time_s, 300.0f);
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            const float gpu_c[3] = {accum[base * 4 + 0], accum[base * 4 + 1],
                                    accum[base * 4 + 2]};
            const float shallow_c[3] = {shallow.color.x(), shallow.color.y(),
                                        shallow.color.z()};
            const float deep_c[3] = {deep.color.x(), deep.color.y(), deep.color.z()};
            for (int c = 0; c < 3; ++c) {
                worst_shallow = std::max(worst_shallow, std::fabs(gpu_c[c] - shallow_c[c]));
                if (deep.hit) {
                    worst_deep = std::max(worst_deep, std::fabs(gpu_c[c] - deep_c[c]));
                }
            }
            EXPECT_NEAR(accum[base * 4 + 3], shallow.alpha, 3.0e-3f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(zsum[base], shallow.ray_t * shallow.alpha,
                        0.02f + 0.01f * std::fabs(shallow.ray_t)) << "x=" << x << " y=" << y;
            ++compared;
        }
    }
    EXPECT_GT(compared, 20) << "覆盖像素太少";
    EXPECT_LT(worst_shallow, 1.0e-3f)
        << "GPU 与浅水 CPU 参考不一致：细节波/求交的 ω 口径分叉了";
    EXPECT_GT(worst_deep, 4.0f * std::max(worst_shallow, 1.0e-3f))
        << "深水 CPU 参考和 GPU 一样：本用例测不出深度口径（阈值失效）";

    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// flow map：1×1 常量纹理 (1, 0) ⇒ flow = +x（强度 1）。
// 与 CPU 参考（flow_x=1, flow_z=0）对拍：证明 GPU 采样/解码与 CPU 口径一致。
TEST(WaterSkinGpu, WavesWithFlowMapMatchReference) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {2.0f, 8.0f, -1.0f};
    cam.target = {2.0f, 0.0f, 4.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.5f;
    const float ior = 1.333f;
    const jpov::WaterWaves waves = MakeTestWaves();

    // RGBA32F 1×1：精确 (1, 0)（避免 8bit 编码的 0.004 误差；RGBA 在 llvmpipe 上最稳）。
    unsigned int flow_tex = 0;
    glGenTextures(1, &flow_tex);
    glBindTexture(GL_TEXTURE_2D, flow_tex);
    const float flow_px[4] = {1.0f, 0.5f, 0.0f, 0.0f};   // 编码 (flow*0.5+0.5)：flow=(1,0)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, flow_px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/flow_tex);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> zsum = ReadRed(f.water.layer().depth_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior,
                                              jpov::water::WaterMediumSample{}, &waves,
                                              /*flow_x=*/1.0f, /*flow_z=*/0.0f);
            if (!e.hit) {
                continue;
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            EXPECT_NEAR(accum[base * 4 + 0], e.color.x(), 4.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 1], e.color.y(), 4.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.color.z(), 4.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], e.alpha, 3.0e-3f);
            EXPECT_NEAR(zsum[base], e.ray_t * e.alpha, 0.02f + 0.01f * std::fabs(e.ray_t));
            ++compared;
        }
    }
    EXPECT_GT(compared, 20) << "覆盖像素太少";

    glDeleteTextures(1, &flow_tex);
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 回归（2026-11-23 报障）：位移主波被足迹截断后，**细节波的法线必须留在画面里**。
//
// 旧版求交的兜底分支（区间两端同号）直接 t=t_plane + 法线留成 (0,1,0)，而且不跑
// AddDetailWaveGradients ⇒ 只要位移波被足迹全部截断，整片水面瞬间退化成完美镜面：
// 「某种波的 pattern 在固定距离生硬消失」，断点随 kWaveLodPxMin 平移
// （2 px→80 m、3 px→63 m，report_cam_75m 实测）。
//
// 判据（只挑「位移波已被截断」的像素，否则测不到这条分支）：
//   ① CPU 参考的法线必须是非平的（细节波还在几何里）；
//   ② GPU debug=1（N*0.5+0.5）必须与 CPU 参考逐像素同口径。
TEST(WaterSkinGpu, FarFieldKeepsDetailNormals) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 8.0f, 0.0f};
    cam.target = {0.0f, 0.0f, 12.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const float time_s = 1.5f;
    const float depth_m = 300.0f;

    jpov::WaterWaves waves;
    waves.enabled = true;
    waves.displacement = true;
    waves.num_waves = 1;
    waves.waves[0] = jpov::WaterWave{1.0f, 0.0f, 0.5f, 0.02f, 0.0f};   // 唯一的位移波
    waves.num_detail_waves = 2;
    waves.detail_waves[0] = jpov::WaterWave{1.0f, 0.0f, 6.0f, 0.05f, 0.0f};
    waves.detail_waves[1] = jpov::WaterWave{0.3f, 0.95f, 4.0f, 0.03f, 1.7f};

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/0, time_s, depth_m, /*wave_debug=*/1);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const Reference ref = BuildReference(cam);
    const float pixel_angle = 2.0f * ref.tan_half_fov / static_cast<float>(kHeight);

    int examined = 0;    // 位移波已被截断的像素数
    int textured = 0;    // 其中法线仍非平的像素数
    float worst = 0.0f;  // GPU vs CPU 法线的最大偏差
    for (int y = 0; y < kHeight; y += 2) {
        for (int x = 0; x < kWidth; x += 2) {
            const Vec3f rd = RayAt(ref, x, y);
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            if (!(rd.y() < -1.0e-3f)) {
                continue;
            }
            const float t_plane = (level - cam.position.y()) / rd.y();
            const float footprint =
                jpov::water::WaveFootprintM(t_plane, rd.y(), pixel_angle);
            if (jpov::water::WaveAmplitudeSum(waves, 0.0f, 0.0f, footprint) != 0.0f) {
                continue;   // 这条像素的位移波还活着 ⇒ 测的不是兜底分支
            }
            WaterPixel px;
            if (!EvaluateWaterPixel(cam.position, ref.fwd, rd, level, ior, depth[i],
                                    cam.near, cam.far, &px, &waves, 0.0f, 0.0f,
                                    pixel_angle, time_s, depth_m)) {
                continue;
            }
            ++examined;
            const float tilt = std::sqrt(px.normal.x() * px.normal.x() +
                                         px.normal.z() * px.normal.z());
            if (tilt > 0.01f) {   // ≈ 0.6°
                ++textured;
            }
            const float gx = accum[i * 4 + 0] * 2.0f - 1.0f;
            const float gy = accum[i * 4 + 1] * 2.0f - 1.0f;
            const float gz = accum[i * 4 + 2] * 2.0f - 1.0f;
            worst = std::max(worst, std::fabs(gx - px.normal.x()));
            worst = std::max(worst, std::fabs(gy - px.normal.y()));
            worst = std::max(worst, std::fabs(gz - px.normal.z()));
        }
    }
    EXPECT_GT(examined, 40) << "没有像素落在「位移波被截断」的区间，用例失效";
    EXPECT_GT(textured, 20)
        << "位移波截断后法线全平 ⇒ 细节波被丢了（远场会变完美镜面 + 硬分界）";
    EXPECT_LT(worst, 0.02f) << "GPU 与 CPU 参考的法线不同口径";

    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 水下 + 调试 program（below_debug）：其余用例只覆盖了 above_debug ——
// FarFieldKeepsDetailNormals 用 wave_debug=1，但相机在水上 ⇒ 走 above_debug。
// 这里在水下用 wave_debug=7（输出 α）跑一次，既证明 below_debug 这条 program 真的能跑，
// 又锁住「水下 α = 平面菲涅尔（含 TIR）」这个调试口径（与生产支路同源）。
TEST(WaterSkinGpu, UnderwaterDebugProgramOutputsAlpha) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, -3.0f, 0.0f};
    cam.target = {0.0f, 0.0f, 0.0f};   // 正上方看：整屏落在 Snell 窗内（F 很小）
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;
    const jpov::water::WaterMediumSample medium = MakeMedium(level, ior);

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);
    f.Render(cam, depth_tex, level, ior, &medium, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, /*waves=*/nullptr,
             /*wave_flow_tex=*/0, /*wave_time_s=*/0.0f, /*wave_depth_m=*/300.0f,
             /*wave_debug=*/7);

    const std::vector<float> accum = ReadRgba(f.water.layer().accum_tex);
    const std::vector<float> reveal = ReadRed(f.water.layer().reveal_tex);
    const Reference ref = BuildReference(cam);

    int compared = 0;
    for (int y = 0; y < kHeight; y += 3) {
        for (int x = 0; x < kWidth; x += 3) {
            const LayerExpect e = ExpectLayer(cam, ref, depth, x, y, level, ior, medium);
            if (!e.hit) {
                continue;
            }
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            // mode 7：oAccum = vec4(alpha, alpha, alpha, 1)，oReveal = 1 ⇒ 混合后 reveal = 0。
            EXPECT_NEAR(accum[base * 4 + 0], e.alpha, 1.0e-3f) << "x=" << x << " y=" << y;
            EXPECT_NEAR(accum[base * 4 + 1], e.alpha, 1.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 2], e.alpha, 1.0e-3f);
            EXPECT_NEAR(accum[base * 4 + 3], 1.0f, 1.0e-3f);
            EXPECT_NEAR(reveal[base], 0.0f, 1.0e-3f);
            ++compared;
        }
    }
    EXPECT_GT(compared, 60) << "水下像素太少，用例没有覆盖面";
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

// 远场开关（2026-11-25）：far_field 必须**只在远处**改变画面（far>0 的像素），
// 近处（far=0）逐字节不变。这是「开关真接上了 + 没污染近场」的最小判据。
// 机位掠射（~2.9°）：屏幕下方看近处（footprint 小 ⇒ far=0），上方贴地平线看远处（far→1）。
TEST(WaterSkinGpu, FarFieldSwitchOnlyAffectsDistantWater) {
    Fixture f;
    f.SetUp();
    Camera cam;
    cam.position = {0.0f, 6.0f, 0.0f};
    cam.target = {0.0f, 0.0f, -40.0f};   // 俯角 ~8.5°，屏幕下部近、上部远
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;
    const float level = 0.0f;
    const float ior = 1.333f;

    const std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);

    jpov::WaterWaves waves = MakeTestWaves();
    waves.far_field = false;
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/0, /*wave_time_s=*/1.3f);
    const std::vector<float> off = ReadRgba(f.water.layer().accum_tex);

    waves.far_field = true;
    f.Render(cam, depth_tex, level, ior, /*medium=*/nullptr, /*has_sun=*/false,
             jpov::Vec3f{0.0f, 1.0f, 0.0f}, jpov::Vec3f{0.0f, 0.0f, 0.0f},
             jpov::lighting::kSunAngularRadiusRad, /*sun_footprint=*/0.0f, &waves,
             /*wave_flow_tex=*/0, /*wave_time_s=*/1.3f);
    const std::vector<float> on = ReadRgba(f.water.layer().accum_tex);

    int opaque = 0;
    int changed = 0;
    int near_opaque = 0;
    int near_changed = 0;
    double sum_on = 0.0;
    bool finite = true;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t base = static_cast<size_t>(y) * kWidth + x;
            if (off[base * 4 + 3] <= 0.0f) {
                continue;
            }
            ++opaque;
            if (!std::isfinite(on[base * 4 + 0]) || !std::isfinite(on[base * 4 + 1]) ||
                !std::isfinite(on[base * 4 + 2])) {
                finite = false;
            }
            sum_on += on[base * 4 + 0] + on[base * 4 + 1] + on[base * 4 + 2];
            const float d = std::max({std::fabs(off[base * 4 + 0] - on[base * 4 + 0]),
                                      std::fabs(off[base * 4 + 1] - on[base * 4 + 1]),
                                      std::fabs(off[base * 4 + 2] - on[base * 4 + 2])});
            if (d > 1.0e-3f) {
                ++changed;
            }
            if (y <= 2) {   // 最低 3 行 = 最近的水（footprint 极小 ⇒ far 必须=0）
                ++near_opaque;
                if (d > 1.0e-6f) {
                    ++near_changed;
                }
            }
        }
    }
    EXPECT_TRUE(finite) << "远场输出出现 NaN/Inf";
    EXPECT_GT(opaque, 500) << "水皮覆盖太少";
    EXPECT_GT(near_opaque, 50) << "近处采样行太少";
    EXPECT_EQ(near_changed, 0) << "近处（最低几行）被远场污染了：far 本应=0，画面必须逐字节不变";
    EXPECT_GT(changed, opaque / 100) << "远场开关几乎没改变画面：far 没有真正生效";
    // 能量健全性：远场不能把画面打成全黑（丢反射）或 NaN。
    EXPECT_GT(sum_on / (3.0 * static_cast<double>(opaque)), 5.0e-3)
        << "远场输出几乎全黑（反射丢了？）";

    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

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

// gl_FragCoord.z 编码（与 MRT#1 同口径）。
//   ndc = ((f+n) − 2·n·f/z) / (f − n)
float NdcOfViewDepth(float z_view) {
    return ((kFar + kNear) - 2.0f * kNear * kFar / z_view) / (kFar - kNear);
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
                        const jpov::water::WaterMediumSample& medium) {
    LayerExpect e;
    const Vec3f rd = RayAt(ref, x, y);
    const size_t i = static_cast<size_t>(y) * kWidth + x;
    WaterPixel px;
    if (!EvaluateWaterPixel(cam.position, ref.fwd, rd, level_y, ior, depth[i],
                            cam.near, cam.far, &px)) {
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
            jpov::water::ReflectDirection(rd));
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
                const jpov::water::WaterMediumSample* medium) {
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

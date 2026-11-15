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

float FragCoordZOfViewDepth(float z_view) {
    return 0.5f * NdcOfViewDepth(z_view) + 0.5f;
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

struct Target {
    unsigned int fbo = 0;
    unsigned int tex = 0;
};

Target MakeTarget() {
    Target t;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kWidth, kHeight, 0, GL_RGBA, GL_FLOAT,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
}

void DestroyTarget(Target* t) {
    if (t->fbo != 0) {
        glDeleteFramebuffers(1, &t->fbo);
        t->fbo = 0;
    }
    if (t->tex != 0) {
        glDeleteTextures(1, &t->tex);
        t->tex = 0;
    }
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

struct Expected {
    bool hit = false;
    float reflectance = 0.0f;
    float view_z = 0.0f;
    Vec3f color = {0.0f, 0.0f, 0.0f};
};

Expected ExpectPixel(const Camera& cam, const Reference& ref,
                     const std::vector<float>& depth, int x, int y,
                     float level_y, float ior, const Vec3f& bg) {
    Expected e;
    const Vec3f rd = RayAt(ref, x, y);
    const size_t i = static_cast<size_t>(y) * kWidth + x;
    WaterPixel px;
    if (!EvaluateWaterPixel(cam.position, ref.fwd, rd, level_y, ior, depth[i],
                            cam.near, cam.far, &px)) {
        e.color = bg;
        return e;
    }
    e.hit = true;
    e.reflectance = px.reflectance;
    e.view_z = px.view_z;
    const bool under = cam.position.y() < level_y;
    if (under) {
        // 水下：反射的是"水下环境"，本版是常量地色（= uTirColor）。
        e.color = Vec3f{bg.x() * (1.0f - px.reflectance) + kTirR * px.reflectance,
                        bg.y() * (1.0f - px.reflectance) + kTirG * px.reflectance,
                        bg.z() * (1.0f - px.reflectance) + kTirB * px.reflectance};
    } else {
        e.color = Vec3f{bg.x() * (1.0f - px.reflectance) + kSkyR * kEnvScale * px.reflectance,
                        bg.y() * (1.0f - px.reflectance) + kSkyG * kEnvScale * px.reflectance,
                        bg.z() * (1.0f - px.reflectance) + kSkyB * kEnvScale * px.reflectance};
    }
    return e;
}

// ───────────────────────── 测试主体 ─────────────────────────

struct Fixture {
    GlEnv& env = GlEnv::Get();
    jpov::ShaderManager shader_mgr;
    WaterSkinRenderer water;
    unsigned int cube = 0;
    Target target;

    void SetUp() {
        env.Ensure();
        cube = MakeEnvCube();
        target = MakeTarget();
        water.Init(&shader_mgr);
    }

    void TearDown() {
        water.Finalize();
        DestroyTarget(&target);
        if (cube != 0) {
            glDeleteTextures(1, &cube);
            cube = 0;
        }
    }

    // 背景 = 已画好的场景颜色（水皮只 over 合成，不采样它）。
    void ClearTarget(float r, float g, float b) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, kWidth, kHeight);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Render(const Camera& cam, unsigned int depth_tex, float level_y, float ior) {
        WaterSkinRenderer::Inputs in;
        in.camera = cam;
        in.full_w = kWidth;
        in.full_h = kHeight;
        in.scene_depth_tex = depth_tex;
        in.env_cube_tex = cube;
        in.env_scale = kEnvScale;
        in.tir_color = {kTirR, kTirG, kTirB};
        in.level_y = level_y;
        in.ior = ior;
        water.Begin(kWidth, kHeight);
        water.Draw(in);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, kWidth, kHeight);
        const GLenum buf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buf);
        water.Composite(kWidth, kHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

TEST(WaterSkinGpu, AboveWaterMatchesCpuModel) {
    Fixture f;
    f.SetUp();
    const Vec3f bg{0.20f, 0.40f, 0.10f};
    f.ClearTarget(bg.x(), bg.y(), bg.z());

    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};   // 俯视 ~34°，画面顶部恰好落到掠射（F 接近 1）
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;

    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);   // 全背景
    const unsigned int depth_tex = MakeDepthTexture(depth);

    f.Render(cam, depth_tex, /*level_y=*/0.0f, /*ior=*/1.333f);
    const std::vector<float> got = ReadRgba(f.target.tex);

    const Reference ref = BuildReference(cam);
    float max_err = 0.0f;
    int hits = 0;
    float f_min = 1.0f;
    float f_max = 0.0f;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const Expected e = ExpectPixel(cam, ref, depth, x, y, 0.0f, 1.333f, bg);
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            for (int c = 0; c < 3; ++c) {
                max_err = std::max(max_err, std::fabs(got[i + c] - e.color[c]));
            }
            if (e.hit) {
                ++hits;
                f_min = std::min(f_min, e.reflectance);
                f_max = std::max(f_max, e.reflectance);
            }
        }
    }
    glDeleteTextures(1, &depth_tex);
    f.TearDown();

    // 全部像素都应打中水面（相机朝下、无遮挡）。
    EXPECT_EQ(hits, kWidth * kHeight);
    // 反构/菲涅尔/WBO 三处走样都会让这里炸（RGBA16F 的量化约 1e-3）。
    EXPECT_LT(max_err, 3.0e-3f) << "max_err=" << max_err;
    // 自检：这一帧确实覆盖了「近正入射 → 掠射」的大范围 F（否则上面的对拍测不到角度依赖）。
    EXPECT_LT(f_min, 0.03f) << "f_min=" << f_min;
    EXPECT_GT(f_max, 0.20f) << "f_max=" << f_max;
}

TEST(WaterSkinGpu, UnderwaterWindowAndTir) {
    Fixture f;
    f.SetUp();
    const Vec3f bg{0.30f, 0.35f, 0.40f};
    f.ClearTarget(bg.x(), bg.y(), bg.z());

    Camera cam;
    cam.position = {0.0f, -8.0f, 0.0f};   // 水下，正上方看
    cam.target = {0.0f, 0.0f, 0.0f};
    // 120° 垂直 FOV：画面中心 = 正上（窗内），四角 ≈ 55° 离竖直（窗外 TIR）。
    // 60° 不够 —— 30° 离竖直仍在 Snell 窗（48.6°）内，一帧里一个 TIR 像素都没有。
    cam.fov = 120.0f;
    cam.near = kNear;
    cam.far = kFar;

    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);

    f.Render(cam, depth_tex, /*level_y=*/0.0f, /*ior=*/1.333f);
    const std::vector<float> got = ReadRgba(f.target.tex);

    const Reference ref = BuildReference(cam);
    float max_err = 0.0f;
    int tir_pixels = 0;
    int window_pixels = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const Expected e = ExpectPixel(cam, ref, depth, x, y, 0.0f, 1.333f, bg);
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            for (int c = 0; c < 3; ++c) {
                max_err = std::max(max_err, std::fabs(got[i + c] - e.color[c]));
            }
            if (e.reflectance >= 1.0f) {
                ++tir_pixels;
            } else {
                ++window_pixels;
            }
        }
    }
    // 画面中心 ≈ 正上方（窗内），四角 = 掠射（窗外 TIR）—— 一帧同时覆盖两段。
    const size_t center = (static_cast<size_t>(kHeight / 2) * kWidth + kWidth / 2) * 4;
    EXPECT_LT(std::fabs(got[center] - (bg.x() * 0.98f + kTirR * 0.02f)), 5.0e-3f);
    EXPECT_GT(tir_pixels, 0);
    EXPECT_GT(window_pixels, 0);
    EXPECT_LT(max_err, 3.0e-3f) << "max_err=" << max_err;
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

TEST(WaterSkinGpu, DepthRejectionLeavesBackgroundUntouched) {
    Fixture f;
    f.SetUp();
    const Vec3f bg{0.20f, 0.40f, 0.10f};
    f.ClearTarget(bg.x(), bg.y(), bg.z());

    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;

    // 左半张图：场景在 1 m（比水面近得多）⇒ 水皮必须整片不画。
    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth / 2; ++x) {
            depth[static_cast<size_t>(y) * kWidth + x] = FragCoordZOfViewDepth(1.0f);
        }
    }
    const unsigned int depth_tex = MakeDepthTexture(depth);

    // RGBA16F 对 0.2 的量化值不是精确 0.2（实测 0.199951），所以基线取
    // 「清屏后、未画水之前」的真实读回值 —— 断言“一个字节都没改”必须对它比。
    const std::vector<float> bg_quantized = ReadRgba(f.target.tex);

    f.Render(cam, depth_tex, /*level_y=*/0.0f, /*ior=*/1.333f);
    const std::vector<float> got = ReadRgba(f.target.tex);

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth / 2; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            ASSERT_NEAR(got[i + 0], bg_quantized[i + 0], 1.0e-6f) << "x=" << x << " y=" << y;
            ASSERT_NEAR(got[i + 1], bg_quantized[i + 1], 1.0e-6f);
            ASSERT_NEAR(got[i + 2], bg_quantized[i + 2], 1.0e-6f);
        }
    }
    // 右半张（背景深度）必须真的被画（否则上面那组断言等于空测）。
    bool right_changed = false;
    for (int y = 0; y < kHeight && !right_changed; ++y) {
        for (int x = kWidth / 2; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            if (std::fabs(got[i + 2] - bg_quantized[i + 2]) > 1.0e-3f) {
                right_changed = true;
                break;
            }
        }
    }
    EXPECT_TRUE(right_changed);
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

TEST(WaterSkinGpu, WboLayerAttachmentsCarryFresnelAndDepth) {
    Fixture f;
    f.SetUp();
    const Vec3f bg{0.20f, 0.40f, 0.10f};

    Camera cam;
    cam.position = {0.0f, 6.0f, 9.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = kNear;
    cam.far = kFar;

    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    const unsigned int depth_tex = MakeDepthTexture(depth);

    WaterSkinRenderer::Inputs in;
    in.camera = cam;
    in.full_w = kWidth;
    in.full_h = kHeight;
    in.scene_depth_tex = depth_tex;
    in.env_cube_tex = f.cube;
    in.env_scale = kEnvScale;
    in.tir_color = {kTirR, kTirG, kTirB};
    in.level_y = 0.0f;
    in.ior = 1.333f;
    f.water.Begin(kWidth, kHeight);
    f.water.Draw(in);

    const std::vector<float> accum = ReadRgba(f.water.accum_tex());
    const std::vector<float> reveal = ReadRed(f.water.reveal_tex());
    const std::vector<float> zsum = ReadRed(f.water.depth_tex());

    const Reference ref = BuildReference(cam);
    int checked = 0;
    for (int y = 0; y < kHeight; y += 5) {
        for (int x = 0; x < kWidth; x += 5) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const Expected e = ExpectPixel(cam, ref, depth, x, y, 0.0f, 1.333f, bg);
            ASSERT_TRUE(e.hit);
            // att0：预乘色 = 反射色 × F；α = F。
            EXPECT_NEAR(accum[i * 4 + 0], kSkyR * kEnvScale * e.reflectance, 1.0e-3f);
            EXPECT_NEAR(accum[i * 4 + 3], e.reflectance, 1.0e-3f);
            // att1：FS 写 α，混合后纹理里是 1 − F。
            EXPECT_NEAR(reveal[i], 1.0f - e.reflectance, 1.0e-3f);
            // att3：z_view × F（雾接入后的「代表深度」就是这个口径）。
            EXPECT_NEAR(zsum[i], e.view_z * e.reflectance, 2.0e-2f);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
    glDeleteTextures(1, &depth_tex);
    f.TearDown();
}

}  // namespace

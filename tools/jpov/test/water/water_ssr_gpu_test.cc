// JPOV 水皮 SSR 的 GPU 测试 —— 锁三件事
//
// ① **query 趟的 depth 口径**（`QueryDepthMatchesAnalyticPlane`）：
//    平水面 + 已知机位 ⇒ 窗口深度必须等于解析射线∩平面再投影的值。
//    这是整条链路唯一的"接口换算"（欧氏 t → 1/z 窗口深度），错了 SSR 的射线
//    起点就会整体飘，倒影整体错位（而且看起来"还能用"，最难查）。
// ② **query 趟的法线口径**（`QueryNormalIsViewSpaceUp`）：
//    平水面 ⇒ 视空间法线必须是 (0,1,0) 经相机基旋转后的结果，且 .a = 1。
//    视空间约定错了会把倒影方向整体旋到别处（2026-11-24 "暗物体倒影被天光洗白"
//    就是同一类错误的另一面）。
// ③ **A = 0 ⇒ 逐字节等于纯探针**（`ZeroCoverageEqualsEnvOnly`）：
//    装置报 A = 0 的像素，着色器必须**完全**走环境探针那条路（不是"乘 0"）。
//    这条同时守住"L 在 A=0 处可能是 NaN/Inf"的防线（乘 0 会污染 env）。
//
// 负向验证（提交前实测过）：
//   · 把 query 的 depth 写成"欧氏 t 原样"⇒ ① 红；
//   · 把视空间法线的 z 符号写反（−dot 写成 +dot）⇒ ② 红；
//   · 把 `if (ssr.a > 0)` 去掉、直接 mix ⇒ ③ 红（NaN 传播）。
//
// 合成输入：不需要场景/物体 —— 水皮的输入只有「相机 + 平面 + 场景深度」，
// SSR 的输入只有「L/A 两张屏幕图」，两者都能直接造出来。

#define GL_GLEXT_PROTOTYPES

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_skin_v2_renderer.h"

namespace {

using jpov::Camera;
using jpov::Vec3f;
using jpov::WaterSsrQueryTextures;
using jpov::WaterSsrQueryTuning;
using jpov::WaterSkinV2Renderer;

constexpr int kWidth = 64;
constexpr int kHeight = 48;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;
constexpr float kFovDeg = 60.0f;

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
        cfg.title = "Water SSR GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

constexpr float kSkyR = 0.50f;
constexpr float kSkyG = 0.60f;
constexpr float kSkyB = 1.00f;

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

// 全 1.0 = 背景（没有不透明几何挡住水面）。
unsigned int MakeSceneDepth() {
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

// 一整张恒值 RGBA32F（SSR 的 L/A 图）。
unsigned int MakeConstantRgba(const float rgba[4]) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    std::vector<float> data(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    for (size_t i = 0; i < data.size(); i += 4) {
        data[i + 0] = rgba[0];
        data[i + 1] = rgba[1];
        data[i + 2] = rgba[2];
        data[i + 3] = rgba[3];
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kWidth, kHeight, 0, GL_RGBA, GL_FLOAT,
                 data.data());
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

// 机位：正上方 4 m 俯视（phi 轴 = +Y），沿 −Z 看 ⇒ 视空间基 = 世界基（右手、z 取反）。
Camera MakeCameraLookingDown() {
    Camera cam;
    cam.position = Vec3f{0.0f, 4.0f, 0.0f};
    cam.target = Vec3f{0.0f, 0.0f, -8.0f};
    cam.up = Vec3f{0.0f, 1.0f, 0.0f};
    cam.fov = kFovDeg;
    cam.near = kNear;
    cam.far = kFar;
    return cam;
}

struct Fixture {
    GlEnv& env = GlEnv::Get();
    jpov::ShaderManager shader_mgr;
    WaterSkinV2Renderer v2;
    unsigned int cube = 0;
    unsigned int scene_depth = 0;

    void SetUp() {
        env.Ensure();
        cube = MakeEnvCube();
        scene_depth = MakeSceneDepth();
        v2.Init(&shader_mgr);
    }
    void TearDown() {
        v2.Finalize();
        if (cube != 0) {
            glDeleteTextures(1, &cube);
            cube = 0;
        }
        if (scene_depth != 0) {
            glDeleteTextures(1, &scene_depth);
            scene_depth = 0;
        }
    }

    WaterSkinV2Renderer::Inputs BaseInputs() {
        WaterSkinV2Renderer::Inputs in;
        in.camera = MakeCameraLookingDown();
        in.full_w = kWidth;
        in.full_h = kHeight;
        in.scene_depth_tex = scene_depth;
        in.env_cube_tex = cube;
        in.env_scale = 1.0f;
        in.level_y = 0.0f;
        in.ior = 1.333f;
        in.medium = nullptr;
        in.has_sun = false;
        in.waves = nullptr;     // 纯平面：query 与解析解可比
        in.wave_time_s = 0.0f;
        in.wave_debug = 0;
        return in;
    }
};

// ① depth 口径：解析射线∩平面 → 投影 → 窗口深度。
TEST(WaterSsrGpu, QueryDepthMatchesAnalyticPlane) {
    Fixture f;
    f.SetUp();
    WaterSkinV2Renderer::Inputs in = f.BaseInputs();
    WaterSsrQueryTuning tuning;
    f.v2.Begin(kWidth, kHeight);
    f.v2.DrawQuery(in, tuning);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    const WaterSsrQueryTextures q = f.v2.query();
    ASSERT_NE(q.depth_tex, 0u);
    ASSERT_EQ(q.width, kWidth);
    const std::vector<float> depth = ReadRed(q.depth_tex);

    const Vec3f pos = in.camera.position;
    const Vec3f fwd = in.camera.target - pos;
    const float fwd_len = fwd.Norm();
    ASSERT_GT(fwd_len, 1.0e-6f);
    const Vec3f fwd_n{fwd.x() / fwd_len, fwd.y() / fwd_len, fwd.z() / fwd_len};
    const Vec3f side0 = Vec3f{fwd_n.y() * in.camera.up.z() - fwd_n.z() * in.camera.up.y(),
                              fwd_n.z() * in.camera.up.x() - fwd_n.x() * in.camera.up.z(),
                              fwd_n.x() * in.camera.up.y() - fwd_n.y() * in.camera.up.x()};
    const float side_len = side0.Norm();
    ASSERT_GT(side_len, 1.0e-6f);
    const Vec3f side{side0.x() / side_len, side0.y() / side_len, side0.z() / side_len};
    const Vec3f upv{side.y() * fwd_n.z() - side.z() * fwd_n.y(),
                    side.z() * fwd_n.x() - side.x() * fwd_n.z(),
                    side.x() * fwd_n.y() - side.y() * fwd_n.x()};
    const float tan_half = std::tan(0.5f * kFovDeg * 3.14159265358979323846f / 180.0f);
    const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);

    int checked = 0;
    for (int py = 4; py < kHeight - 4; py += 7) {
        for (int px = 4; px < kWidth - 4; px += 9) {
            // glReadPixels 的原点在左下；像素中心 NDC。
            const float ndc_x = ((static_cast<float>(px) + 0.5f) / kWidth) * 2.0f - 1.0f;
            const float ndc_y = ((static_cast<float>(py) + 0.5f) / kHeight) * 2.0f - 1.0f;
            const Vec3f dir{side.x() * (ndc_x * tan_half * aspect) +
                                upv.x() * (ndc_y * tan_half) + fwd_n.x(),
                            side.y() * (ndc_x * tan_half * aspect) +
                                upv.y() * (ndc_y * tan_half) + fwd_n.y(),
                            side.z() * (ndc_x * tan_half * aspect) +
                                upv.z() * (ndc_y * tan_half) + fwd_n.z()};
            // 射向水面 y = 0（相机在上方 ⇒ dir.y < 0）。
            if (!(dir.y() < -1.0e-4f)) {
                continue;
            }
            const float t = (in.level_y - pos.y()) / dir.y();
            if (!(t > 0.0f)) {
                continue;
            }
            const Vec3f p{pos.x() + dir.x() * t, pos.y() + dir.y() * t, pos.z() + dir.z() * t};
            // 视轴深度（相机的 +Z 是 −fwd）。
            const float z_eye = (p.x() - pos.x()) * fwd_n.x() +
                                (p.y() - pos.y()) * fwd_n.y() +
                                (p.z() - pos.z()) * fwd_n.z();
            ASSERT_GT(z_eye, 0.0f);
            const float ndc_z = (kFar + kNear - 2.0f * kFar * kNear / z_eye) /
                                (kFar - kNear);
            const float expect = std::min(0.5f * ndc_z + 0.5f, 1.0f);
            const float got = depth[static_cast<size_t>(py) * kWidth + px];
            // 允许 0.2% 相对误差：R32F 存储 + 两条不同的算术路径。
            ASSERT_NEAR(got, expect, std::max(2.0e-3f, 0.002f * expect))
                << "px=" << px << " py=" << py << " (期望 " << expect << ")";
            ++checked;
        }
    }
    EXPECT_GT(checked, 10) << "抽查像素太少（相机/平面参数不对）";
    f.TearDown();
}

// ② 视空间法线：平水面 ⇒ N_view = (0,1,0) 在相机基上的坐标，且 .a = 1。
TEST(WaterSsrGpu, QueryNormalIsViewSpaceUp) {
    Fixture f;
    f.SetUp();
    WaterSkinV2Renderer::Inputs in = f.BaseInputs();
    WaterSsrQueryTuning tuning;
    f.v2.Begin(kWidth, kHeight);
    f.v2.DrawQuery(in, tuning);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    const WaterSsrQueryTextures q = f.v2.query();
    ASSERT_NE(q.normal_tex, 0u);
    const std::vector<float> nrm = ReadRgba(q.normal_tex);

    // 相机基（与 BuildLookAt 同源：行 = side / up / −fwd）。
    const Vec3f pos = in.camera.position;
    const Vec3f fwd0 = in.camera.target - pos;
    const float fl = fwd0.Norm();
    const Vec3f fwd{fwd0.x() / fl, fwd0.y() / fl, fwd0.z() / fl};
    const Vec3f side0{fwd.y() * in.camera.up.z() - fwd.z() * in.camera.up.y(),
                      fwd.z() * in.camera.up.x() - fwd.x() * in.camera.up.z(),
                      fwd.x() * in.camera.up.y() - fwd.y() * in.camera.up.x()};
    const float sl = side0.Norm();
    const Vec3f side{side0.x() / sl, side0.y() / sl, side0.z() / sl};
    const Vec3f upv{side.y() * fwd.z() - side.z() * fwd.y(),
                    side.z() * fwd.x() - side.x() * fwd.z(),
                    side.x() * fwd.y() - side.y() * fwd.x()};
    // 世界法线 (0,1,0) 在 (side, upv, −fwd) 上的坐标：dot(N,side)=side.y 等。
    const float expect_x = side.y();
    const float expect_y = upv.y();
    const float expect_z = -fwd.y();

    int checked = 0;
    for (int py = 6; py < kHeight - 6; py += 9) {
        for (int px = 6; px < kWidth - 6; px += 11) {
            const size_t i = (static_cast<size_t>(py) * kWidth + px) * 4;
            if (nrm[i + 3] < 0.5f) {
                continue;   // 水面之外（本机位不该有，但别在这里断言）
            }
            EXPECT_NEAR(nrm[i + 0], expect_x, 1.0e-3);
            EXPECT_NEAR(nrm[i + 1], expect_y, 1.0e-3);
            EXPECT_NEAR(nrm[i + 2], expect_z, 1.0e-3);
            ++checked;
        }
    }
    EXPECT_GT(checked, 10) << "抽查像素太少";
    f.TearDown();
}

// ③ A = 0 ⇒ 与"没有 SSR"逐字节相同（并且 NaN 的 L 不许污染 env）。
TEST(WaterSsrGpu, ZeroCoverageEqualsEnvOnly) {
    Fixture f;
    f.SetUp();
    WaterSkinV2Renderer::Inputs in = f.BaseInputs();
    WaterSsrQueryTuning tuning;

    // 基线：完全没有 SSR 纹理。
    f.v2.Begin(kWidth, kHeight);
    f.v2.Draw(in, /*sample_count=*/4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const std::vector<float> base = ReadRgba(f.v2.layer().accum_tex);

    // A = 0，L 里塞 **NaN**（模拟"未覆盖像素的 rgb 无意义"）⇒ 必须逐字节不变。
    // 用 1e30 测不出问题（有限值乘 0 仍是 0）；NaN 乘 0 才是 NaN —— 那才是这条防线
    // 真正要挡的东西（升采样在空区域可能产出 NaN/Inf）。
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    const float zero_a[4] = {kNaN, kNaN, kNaN, 0.0f};
    const unsigned int zero_tex = MakeConstantRgba(zero_a);
    WaterSkinV2Renderer::Inputs in_zero = in;
    in_zero.ssr_reflection_tex = zero_tex;
    f.v2.Begin(kWidth, kHeight);
    f.v2.Draw(in_zero, /*sample_count=*/4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const std::vector<float> got = ReadRgba(f.v2.layer().accum_tex);

    ASSERT_EQ(got.size(), base.size());
    size_t differing = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != base[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(differing, 0u) << "A=0 时必须逐字节等于纯探针（否则 L 的垃圾值在污染 env）";

    // 正向对照：A = 1 + 纯黑 L ⇒ 反射项被整条替换掉，画面必须明显变暗。
    //（只判方向，不判具体数值：F/多散射口径不在本测试的职责范围内。）
    const float black_full[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const unsigned int black_tex = MakeConstantRgba(black_full);
    WaterSkinV2Renderer::Inputs in_black = in;
    in_black.ssr_reflection_tex = black_tex;
    f.v2.Begin(kWidth, kHeight);
    f.v2.Draw(in_black, /*sample_count=*/4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const std::vector<float> dark = ReadRgba(f.v2.layer().accum_tex);

    double sum_base = 0.0;
    double sum_dark = 0.0;
    for (size_t i = 0; i < base.size(); ++i) {
        sum_base += base[i];
        sum_dark += dark[i];
    }
    EXPECT_LT(sum_dark, 0.9 * sum_base) << "A=1 且 L=0 时反射项必须真的被替换掉";

    glDeleteTextures(1, &zero_tex);
    glDeleteTextures(1, &black_tex);
    f.TearDown();
}

// ④ 法线钳位：开 ⇒ 每个被查询像素的**反射方向** y ≥ sin(1°)；关 ⇒ 一定存在低于它的像素。
//    这是"魔改法线让 PQ 的 y 半正定"这条逻辑的唯一判据（着色器里的其余行为不变）。
TEST(WaterSsrGpu, QueryNormalClampKeepsRayAboveHorizon) {
    Fixture f;
    f.SetUp();
    WaterSkinV2Renderer::Inputs in = f.BaseInputs();
    // 掠射机位 + 波浪：不钳位时必然有一批法线把反射方向打到水面以下。
    in.camera.position = Vec3f{0.0f, 1.5f, 9.0f};
    in.camera.target = Vec3f{0.0f, 0.0f, -20.0f};
    jpov::WaterWaves waves;
    waves.v2 = true;
    waves.enabled = true;
    waves.num_waves = 3;
    waves.waves[0] = jpov::WaterWave{1.0f, 0.0f, 9.0f, 0.6f, 0.2f};
    waves.waves[1] = jpov::WaterWave{0.4f, 0.9f, 5.0f, 0.35f, 1.4f};
    waves.waves[2] = jpov::WaterWave{-0.7f, 0.7f, 3.0f, 0.2f, 2.2f};
    in.waves = &waves;

    // 相机基（= BuildLookAt 的行：side / upv / −fwd），用来把视空间法线转回世界。
    const Vec3f pos = in.camera.position;
    const Vec3f fwd0 = in.camera.target - pos;
    const float fl = fwd0.Norm();
    const Vec3f fwd{fwd0.x() / fl, fwd0.y() / fl, fwd0.z() / fl};
    const Vec3f side0{fwd.y() * in.camera.up.z() - fwd.z() * in.camera.up.y(),
                      fwd.z() * in.camera.up.x() - fwd.x() * in.camera.up.z(),
                      fwd.x() * in.camera.up.y() - fwd.y() * in.camera.up.x()};
    const float sl = side0.Norm();
    const Vec3f side{side0.x() / sl, side0.y() / sl, side0.z() / sl};
    const Vec3f upv{side.y() * fwd.z() - side.z() * fwd.y(),
                    side.z() * fwd.x() - side.x() * fwd.z(),
                    side.x() * fwd.y() - side.y() * fwd.x()};
    const float tan_half = std::tan(0.5f * kFovDeg * 3.14159265358979323846f / 180.0f);
    const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    const float kSin1 = std::sin(1.0f * 3.14159265358979323846f / 180.0f);

    const auto count_below = [&](bool clamp_on) {
        WaterSsrQueryTuning tuning;
        tuning.clamp_normal_above_horizon = clamp_on;
        f.v2.Begin(kWidth, kHeight);
        f.v2.DrawQuery(in, tuning);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const std::vector<float> nrm = ReadRgba(f.v2.query().normal_tex);
        int below = 0;
        int checked = 0;
        for (int py = 0; py < kHeight; ++py) {
            for (int px = 0; px < kWidth; ++px) {
                const size_t i = (static_cast<size_t>(py) * kWidth + px) * 4;
                if (nrm[i + 3] < 0.5f) {
                    continue;
                }
                // 视空间法线 → 世界；再和世界视线方向做反射。
                const Vec3f n{nrm[i] * side.x() + nrm[i + 1] * upv.x() - nrm[i + 2] * fwd.x(),
                              nrm[i] * side.y() + nrm[i + 1] * upv.y() - nrm[i + 2] * fwd.y(),
                              nrm[i] * side.z() + nrm[i + 1] * upv.z() - nrm[i + 2] * fwd.z()};
                const float ndc_x = ((static_cast<float>(px) + 0.5f) / kWidth) * 2.0f - 1.0f;
                const float ndc_y = ((static_cast<float>(py) + 0.5f) / kHeight) * 2.0f - 1.0f;
                Vec3f rd{side.x() * (ndc_x * tan_half * aspect) + upv.x() * (ndc_y * tan_half) +
                             fwd.x(),
                         side.y() * (ndc_x * tan_half * aspect) + upv.y() * (ndc_y * tan_half) +
                             fwd.y(),
                         side.z() * (ndc_x * tan_half * aspect) + upv.z() * (ndc_y * tan_half) +
                             fwd.z()};
                const float rl = rd.Norm();
                rd = Vec3f{rd.x() / rl, rd.y() / rl, rd.z() / rl};
                const float dn = rd.x() * n.x() + rd.y() * n.y() + rd.z() * n.z();
                const float ry = rd.y() - 2.0f * dn * n.y();
                ++checked;
                if (ry < kSin1 - 2.0e-3f) {
                    ++below;
                }
            }
        }
        return std::pair<int, int>{below, checked};
    };

    const std::pair<int, int> off = count_below(false);
    EXPECT_GT(off.first, 0) << "关掉钳位时必须有反射方向低于地平线的像素（否则本测试测不出东西）";
    const std::pair<int, int> on = count_below(true);
    EXPECT_GT(on.second, 100);
    EXPECT_EQ(on.first, 0) << "开钳位后反射方向的 y 必须半正定（≥ sin1°），实际有 "
                           << on.first << "/" << on.second << " 个像素低于它";
    f.TearDown();
}

}  // namespace

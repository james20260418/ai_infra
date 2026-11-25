// JPOV Water Skin Gold —— 共享场景 / 机位 / GL harness（generator 与 test 共用）
//
// 目的（重构保护）：水皮 FS 要拆成「水上 / 水下 × 生产 / 调试」四个 program，共享一个
//   WaterSurface 结构体。重构必须**逐像素不变**，但光靠 water_skin_gpu_test（48×32、
//   CPU↔GPU 对拍）覆盖不到「整帧观感」。本 gold 用 1280×720 拍 6 张固定机位
//   （水上 ×3 / 水下 ×3，掠射 / 45° / 俯视），作为拆分前后逐像素不变的端到端基线。
//
// 为什么直接用 WaterSkinRenderer 而不是天光查看器场景：
//   ① 本重构只动水皮这一层（不碰介质 / 合成趟），直接驱动这一层最 surgical；
//   ② 水下机位（相机 y < level）查看器没有对应入口，这里手写 Camera 就能覆盖；
//   ③ 输入（env cubemap / 场景深度 / 介质）全在测试里造，不依赖模型资产，快且稳。
//
// 画幅与语义：
//   · env cubemap = 垂直渐变（天→地），使反射颜色随方向连续变化 —— 反射方向 / 地平线夹 /
//     探针 LOD 任何一处走样都会改变颜色，比纯色面更能抓回归。
//   · 场景深度恒 1.0（背景）⇒ 无水皮遮挡，水皮盖满整个视口。
//   · 水上 3 张带太阳（解析 GGX 盘像，moderate 辐照度，clamp 到 [0,1] 输出）；
//     水下 3 张带介质（深海积分反射 S_P(∞)）。
//   · 波浪关（纯平面）：波浪求交/法线由 water_skin_gpu_test 逐像素钉住，本 gold 只护
//     program 拆分与 shared-struct 的等价性。

#ifndef JPOV_TEST_WATER_JPOV_WATER_SKIN_GOLD_COMMON_H_
#define JPOV_TEST_WATER_JPOV_WATER_SKIN_GOLD_COMMON_H_

#define GL_GLEXT_PROTOTYPES

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/lighting/ggx_specular.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_medium_math.h"
#include "tools/jpov/src/water/water_skin_renderer.h"

namespace jpov_water_skin_gold {

// ── 画幅 ──
inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 720;
inline constexpr float kNear = 0.1f;
inline constexpr float kFar = 1000.0f;

// 水面高度与折射率（水上/水下共用）。
inline constexpr float kLevelY = 0.0f;
inline constexpr float kIor = 1.333f;

// 环境探针分辨率（每面）。128² 与真探针一致；渐变下 LINEAR 采样已足够平滑。
inline constexpr int kEnvFaceSize = 128;

// ── 一帧的全部自变量 ──
struct SkinGoldShot {
    const char* name = "";
    jpov::Vec3f cam_pos;
    jpov::Vec3f cam_target;
    float fov = 60.0f;
    bool under = false;   // 相机在水下（cam.y < level_y）
};

// 6 张：水上 ×3（掠射 / 45° / 俯视）+ 水下 ×3（掠射 / 45° / 正上方）。
inline constexpr SkinGoldShot kShots[] = {
    // ── 水上：掠射（仰角 ~2.3°，Fresnel 掠射峰 + 地平线夹取）──
    {"above_graze", /*pos*/ {0.0f, 1.0f, 0.0f}, /*tgt*/ {0.0f, -0.2f, -30.0f}, 60.0f, false},
    // ── 水上：45° 俯视（中段 Fresnel + 反射方向连续性）──
    {"above_45deg", /*pos*/ {0.0f, 15.0f, 15.0f}, /*tgt*/ {0.0f, 0.0f, 0.0f}, 60.0f, false},
    // ── 水上：正俯视（Fresnel 最小 ~2%，反射几乎不可见）──
    {"above_topdown", /*pos*/ {0.0f, 30.0f, 0.0f}, /*tgt*/ {0.0f, 0.0f, 0.0f}, 60.0f, false},
    // ── 水下：掠射（几乎全 TIR 环，只剩一条细 Snell 窗）──
    {"below_graze", /*pos*/ {0.0f, -1.0f, 0.0f}, /*tgt*/ {0.0f, 0.0f, -30.0f}, 60.0f, true},
    // ── 水下：45° 仰视（Snell 窗边缘 + 部分 TIR）──
    {"below_45deg", /*pos*/ {0.0f, -10.0f, -10.0f}, /*tgt*/ {0.0f, 0.0f, 0.0f}, 60.0f, true},
    // ── 水下：正上方（整屏 Snell 窗，深海积分 + Fresnel 薄膜）──
    {"below_up", /*pos*/ {0.0f, -10.0f, 0.0f}, /*tgt*/ {0.0f, 0.0f, 0.0f}, 60.0f, true},
};
inline constexpr int kShotCount = static_cast<int>(sizeof(kShots) / sizeof(kShots[0]));

inline std::string ShotFileName(const SkinGoldShot& shot) {
    return std::string("water_skin_gold_") + shot.name + "_1280x720.png";
}

// ── 环境探针：垂直渐变 cubemap（天→地）──
// 上半球（+Y）偏天蓝，下半球（−Y）偏深蓝灰；颜色随方向连续 ⇒ 反射方向走样会立刻改色。
inline unsigned int MakeGradientEnvCube() {
    const float sky[3] = {0.50f, 0.60f, 1.00f};
    const float ground[3] = {0.01f, 0.02f, 0.04f};

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);

    std::vector<float> face(static_cast<size_t>(kEnvFaceSize) * kEnvFaceSize * 4);
    for (int f = 0; f < 6; ++f) {
        const GLenum face_enum = GL_TEXTURE_CUBE_MAP_POSITIVE_X + f;
        for (int y = 0; y < kEnvFaceSize; ++y) {
            for (int x = 0; x < kEnvFaceSize; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / kEnvFaceSize * 2.0f - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) / kEnvFaceSize * 2.0f - 1.0f;
                float dx = 0.0f, dy = 0.0f, dz = 0.0f;
                switch (f) {
                    case 0: dx = 1.0f; dy = -v; dz = -u; break;   // +X
                    case 1: dx = -1.0f; dy = -v; dz = u; break;   // -X
                    case 2: dx = u; dy = 1.0f; dz = v; break;     // +Y
                    case 3: dx = u; dy = -1.0f; dz = -v; break;   // -Y
                    case 4: dx = u; dy = -v; dz = 1.0f; break;    // +Z
                    default: dx = -u; dy = -v; dz = -1.0f; break; // -Z
                }
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len;
                dy /= len;
                dz /= len;
                const float t = std::clamp(0.5f * dy + 0.5f, 0.0f, 1.0f);
                const size_t i = (static_cast<size_t>(y) * kEnvFaceSize + x) * 4;
                face[i + 0] = ground[0] + (sky[0] - ground[0]) * t;
                face[i + 1] = ground[1] + (sky[1] - ground[1]) * t;
                face[i + 2] = ground[2] + (sky[2] - ground[2]) * t;
                face[i + 3] = 1.0f;
            }
        }
        glTexImage2D(face_enum, 0, GL_RGBA32F, kEnvFaceSize, kEnvFaceSize, 0, GL_RGBA,
                     GL_FLOAT, face.data());
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return tex;
}

// 场景深度：全背景（1.0）⇒ 水皮不遮挡，盖满视口。
inline unsigned int MakeBackgroundDepthTexture() {
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

// 水下介质的固定配置（与 water_skin_gpu_test 的 MakeMedium 同源）。
inline jpov::water::WaterMediumSample MakeMedium() {
    jpov::water::WaterMediumSample m;
    m.level_y = kLevelY;
    m.ior = kIor;
    m.sigma_a = {0.35f, 0.054f, 0.014f};
    m.sigma_s = {0.05f, 0.05f, 0.05f};
    m.phase_g = 0.35f;
    m.ambient = {0.45f, 0.55f, 0.70f};
    m.sun_color = {2.0f, 1.9f, 1.7f};
    m.sun_toward = {0.5f, 0.7071f, 0.5f};
    m.has_sun = true;
    return m;
}

inline jpov::Vec3f NormalizeOr(const jpov::Vec3f& v, const jpov::Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1.0e-8f) {
        return fallback;
    }
    return jpov::Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

// ── GL context（headless，进程内单例）──
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
        cfg.title = "JPOV Water Skin Gold";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// ── harness：驱动 WaterSkinRenderer 拍一张、读回 accum 转 RGBA8 ──
//
// 输出语义：RGBA8 里 alpha = 255 表示「该像素有水皮覆盖」（accum.a > 0），否则 0；
//   RGB = clamp(accum.rgb, 0, 1) × 255（太阳项是 HDR，clamp 到 LDR，generator/test 同一
//   口径 ⇒ 逐位一致）。
struct GoldHarness {
    jpov::ShaderManager shader_mgr;
    jpov::WaterSkinRenderer water;
    unsigned int cube = 0;
    unsigned int depth_tex = 0;
    jpov::water::WaterMediumSample medium = MakeMedium();

    void SetUp() {
        GlEnv::Get().Ensure();
        cube = MakeGradientEnvCube();
        depth_tex = MakeBackgroundDepthTexture();
        water.Init(&shader_mgr);
    }

    void TearDown() {
        water.Finalize();
        if (cube != 0) {
            glDeleteTextures(1, &cube);
            cube = 0;
        }
        if (depth_tex != 0) {
            glDeleteTextures(1, &depth_tex);
            depth_tex = 0;
        }
    }

    // 读 accum 纹理（RGBA16F）→ GL_FLOAT。
    std::vector<float> ReadAccum() const {
        unsigned int fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               water.layer().accum_tex, 0);
        const GLenum buf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buf);
        std::vector<float> out(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, out.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return out;
    }

    // 拍一张 → RGBA8（alpha 标记覆盖，RGB 为 clamp 后的层色）。
    std::vector<unsigned char> RenderShot(const SkinGoldShot& shot) {
        jpov::Camera cam;
        cam.position = shot.cam_pos;
        cam.target = shot.cam_target;
        cam.fov = shot.fov;
        cam.near = kNear;
        cam.far = kFar;

        // 把太阳放到**屏幕中心的反射方向**（与 water_skin_gpu_test::AboveWaterSunDiskMatchesCpu
        // 同口径）：保证日盘像落在画面正中，既让 sun_spec 项真的被拍到，也让 gold 护住它。
        const jpov::Vec3f fwd = NormalizeOr(
            jpov::Vec3f(cam.target.x() - cam.position.x(),
                        cam.target.y() - cam.position.y(),
                        cam.target.z() - cam.position.z()),
            jpov::Vec3f(0.0f, 0.0f, -1.0f));
        const jpov::Vec3f sun_toward = NormalizeOr(
            jpov::Vec3f(fwd.x(), -fwd.y(), fwd.z()), jpov::Vec3f(0.0f, 1.0f, 0.0f));

        jpov::WaterSkinRenderer::Inputs in;
        in.camera = cam;
        in.full_w = kWidth;
        in.full_h = kHeight;
        in.scene_depth_tex = depth_tex;
        in.env_cube_tex = cube;
        in.env_scale = 1.0f;
        in.level_y = kLevelY;
        in.ior = kIor;
        in.medium = shot.under ? &medium : nullptr;
        in.waves = nullptr;
        in.wave_time_s = 0.0f;
        in.wave_depth_m = 300.0f;
        in.wave_debug = 0;
        // 太阳：只给水上（水下支路不吃 sun_spec；介质自带 sun）。方向 = 屏幕中心反射方向。
        in.has_sun = !shot.under;
        in.sun_toward = sun_toward;
        in.sun_irradiance = {2.0f, 1.9f, 1.7f};
        in.sun_angular_radius = jpov::lighting::kSunAngularRadiusRad;
        in.sun_footprint = 1.0f;

        water.Begin(kWidth, kHeight);
        water.Draw(in);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const std::vector<float> accum = ReadAccum();
        std::vector<unsigned char> out(static_cast<size_t>(kWidth) * kHeight * 4, 0);
        for (int i = 0; i < kWidth * kHeight; ++i) {
            const float a = accum[static_cast<size_t>(i) * 4 + 3];
            out[static_cast<size_t>(i) * 4 + 3] = (a > 1.0e-6f) ? 255 : 0;
            for (int c = 0; c < 3; ++c) {
                const float v = accum[static_cast<size_t>(i) * 4 + c];
                const int q = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                out[static_cast<size_t>(i) * 4 + c] = static_cast<unsigned char>(q);
            }
        }
        return out;
    }
};

}  // namespace jpov_water_skin_gold

#endif  // JPOV_TEST_WATER_JPOV_WATER_SKIN_GOLD_COMMON_H_

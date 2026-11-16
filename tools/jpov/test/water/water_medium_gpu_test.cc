// JPOV 水体介质 GPU 测试 —— 合成趟（逐通道透射的两次 draw）× CPU 参考模型逐像素对拍。
//
// 锁的是什么：
//   ① 射线重建 + z_op 计算（合成趟用 uInvVP 反投影，这条路只有上屏才跑到）；
//   ② MediumAt 与 water_medium_math.h::WaterAt 的"同源"是否真的同源（两处公式必须一致）；
//   ③ **两次 draw 的合成代数**：dst = L + dst·T（先乘后加），固定管线做不到单次完成；
//   ④ 介质与**透明层**的交替：层之前的介质只能积到层深度（不是整列），这是
//      "把水体当成一层"那个错法的直接反例。
//
// 场景刻意取最简：identity 的 view_proj ⇒ p_scene = (ndc.x, ndc.y, 1)，
// 深度纹理给 0 ⇒ 哑元（dndc = 1 ⇒ 远平面）。这样 CPU 端可以逐像素复算同一个 z_op。

#define GL_GLEXT_PROTOTYPES

#include <cmath>
#include <memory>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/fog/fog_composite_renderer.h"
#include "tools/jpov/src/water/water_medium_math.h"
#include "tools/jpov/src/water/water_medium_shader.h"

namespace {

constexpr int kWidth = 32;
constexpr int kHeight = 16;
constexpr float kBg[3] = {0.20f, 0.30f, 0.40f};

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
        cfg.title = "Water Medium GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// 一层：色 c、覆盖率 alpha、代表深度 z（视空间当量；本测试里按"沿视线距离"使用）。
jpov::TransparentLayer MakeLayer(float cr, float cg, float cb, float alpha, float z) {
    unsigned int tex[3] = {0, 0, 0};
    glGenTextures(3, tex);
    const auto fill = [](unsigned int id, GLint internal, GLenum format, float r,
                         float g, float b, float a) {
        // 单通道（GL_RED）只需 W*H 个 float：按 RGBA 填再传 GL_RED 会把不同像素的
        // 通道当成不同 texel（只有 texel 0 对）—— 本用例逐像素读回，会把这种错暴露成
        // “只有最左列对”。
        const int comps = (format == GL_RED) ? 1 : 4;
        std::vector<float> px(static_cast<size_t>(kWidth) * kHeight * comps);
        for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
            if (comps == 1) {
                px[i] = r;
            } else {
                px[i * 4 + 0] = r;
                px[i * 4 + 1] = g;
                px[i * 4 + 2] = b;
                px[i * 4 + 3] = a;
            }
        }
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, kWidth, kHeight, 0, format, GL_FLOAT,
                     px.data());
    };
    fill(tex[0], GL_RGBA16F, GL_RGBA, cr * alpha, cg * alpha, cb * alpha, alpha);
    fill(tex[1], GL_R16F, GL_RED, 1.0f - alpha, 0.0f, 0.0f, 0.0f);
    fill(tex[2], GL_R32F, GL_RED, alpha * z, 0.0f, 0.0f, 0.0f);
    glBindTexture(GL_TEXTURE_2D, 0);
    jpov::TransparentLayer layer;
    layer.accum_tex = tex[0];
    layer.reveal_tex = tex[1];
    layer.depth_tex = tex[2];
    return layer;
}

struct Fixture {
    jpov::FogCompositeRenderer composite;
    jpov::ShaderManager shader_mgr;
    unsigned int fbo = 0;
    unsigned int color_tex = 0;
    std::vector<unsigned int> textures;
    std::vector<float> pixels;   // W*H*4，行序自下向上（= glReadPixels）

    void Setup() {
        GlEnv::Get().Ensure();
        composite.Init(&shader_mgr, jpov::water::kWaterMediumGlsl);

        glGenTextures(1, &color_tex);
        glBindTexture(GL_TEXTURE_2D, color_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kWidth, kHeight, 0, GL_RGBA, GL_FLOAT,
                     nullptr);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               color_tex, 0);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        pixels.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    }

    ~Fixture() {
        composite.Finalize();
        for (unsigned int t : textures) {
            glDeleteTextures(1, &t);
        }
        if (fbo != 0) {
            glDeleteFramebuffers(1, &fbo);
        }
        if (color_tex != 0) {
            glDeleteTextures(1, &color_tex);
        }
    }

    jpov::TransparentLayer Track(const jpov::TransparentLayer& layer) {
        textures.push_back(layer.accum_tex);
        textures.push_back(layer.reveal_tex);
        textures.push_back(layer.depth_tex);
        return layer;
    }

    // 清背景 → 跑一趟合成（带介质）→ 读回整幅。
    void Run(const jpov::TransparentLayerSet& layers,
             const jpov::water::WaterMediumSample& medium, jpov::Camera* cam_out) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(kBg[0], kBg[1], kBg[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        jpov::Camera cam;
        cam.position = {0.0f, 5.0f, 0.0f};   // 水面在 y = 0 ⇒ 相机在水上 5m
        cam.target = {0.0f, 5.0f, -1.0f};
        cam.near = 0.1f;
        cam.far = 1000.0f;
        float view_proj[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 1.0f};
        jpov::FogParams params;
        composite.Draw(jpov::FogField{}, /*scene_depth_tex=*/0, cam, view_proj, params,
                       layers, &medium);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, pixels.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (cam_out != nullptr) {
            *cam_out = cam;
        }
    }

    // 像素 (px, py)（py 自下向上）的 CPU 端射线重建 —— 与合成趟的 uInvVP 反投影同式。
    static void RayOf(int px, int py, const jpov::Vec3f& ro, jpov::Vec3f* rd,
                      float* z_op) {
        const float ndc_x = 2.0f * ((static_cast<float>(px) + 0.5f) / kWidth) - 1.0f;
        const float ndc_y = 2.0f * ((static_cast<float>(py) + 0.5f) / kHeight) - 1.0f;
        // identity VP + 深度纹理 1.0 ⇒ p_scene = (ndc.x, ndc.y, 1)
        const jpov::Vec3f p_scene = {ndc_x, ndc_y, 1.0f};
        const float dx = p_scene.x() - ro.x();
        const float dy = p_scene.y() - ro.y();
        const float dz = p_scene.z() - ro.z();
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        *z_op = len;
        *rd = {dx / len, dy / len, dz / len};
    }
};

void FillFloat3(jpov::Vec3f* v, float r, float g, float b) {
    v->x() = r;
    v->y() = g;
    v->z() = b;
}

jpov::water::WaterMediumSample PureWater() {
    jpov::water::WaterMediumSample m;
    m.level_y = 0.0f;
    m.ior = 1.333f;
    FillFloat3(&m.sigma_a, 0.349f, 0.054f, 0.014f);
    FillFloat3(&m.sigma_s, 0.05f, 0.05f, 0.05f);
    m.phase_g = 0.35f;
    FillFloat3(&m.ambient, 0.30f, 0.32f, 0.35f);
    FillFloat3(&m.sun_color, 3.0f, 3.0f, 3.0f);
    FillFloat3(&m.sun_toward, 0.35f, 0.80f, 0.20f);
    m.has_sun = true;
    // sun_toward 的接口约定是单位向量；测试数据也必须是单位长度，否则折射后的
    // B 与 d0/u_w.y 都会算错（生产侧由 renderer.cc 归一）。
    const jpov::Vec3f st = m.sun_toward;
    const float sl = std::sqrt(st.x() * st.x() + st.y() * st.y() + st.z() * st.z());
    FillFloat3(&m.sun_toward, st.x() / sl, st.y() / sl, st.z() / sl);
    return m;
}

}  // namespace

// σ = 0：两次 draw 的逐通道代数必须退化成恒等（背景一字不变）。
TEST(WaterMediumGpu, ZeroSigmaKeepsBackground) {
    Fixture f;
    f.Setup();
    jpov::water::WaterMediumSample m = PureWater();
    FillFloat3(&m.sigma_a, 0.0f, 0.0f, 0.0f);
    FillFloat3(&m.sigma_s, 0.0f, 0.0f, 0.0f);
    f.Run(jpov::TransparentLayerSet{}, m, nullptr);
    for (int i = 0; i < kWidth * kHeight; ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(f.pixels[static_cast<size_t>(i) * 4 + c], kBg[c], 2.0e-3f)
                << "pixel " << i << " channel " << c;
        }
    }
}

// 主用例：逐像素 out = S + bg·exp(−τ)，与 CPU WaterAt 对拍。
TEST(WaterMediumGpu, MatchesCpuReferencePerPixel) {
    Fixture f;
    f.Setup();
    const jpov::water::WaterMediumSample m = PureWater();
    f.Run(jpov::TransparentLayerSet{}, m, nullptr);

    int compared = 0;   // 真正落进水体的像素数（全在天空行的像素不校验也不该为零）
    for (int py = 0; py < kHeight; ++py) {
        for (int px = 0; px < kWidth; ++px) {
            const jpov::Vec3f ro = {0.0f, 5.0f, 0.0f};
            jpov::Vec3f rd;
            float z_op = 0.0f;
            Fixture::RayOf(px, py, ro, &rd, &z_op);
            jpov::Vec3f tau;
            jpov::Vec3f S;
            jpov::water::WaterAt(m, ro, rd, z_op, &tau, &S);
            const float t_r = std::exp(-tau.x());
            const float t_g = std::exp(-tau.y());
            const float t_b = std::exp(-tau.z());
            const float expected[3] = {S.x() + kBg[0] * t_r, S.y() + kBg[1] * t_g,
                                       S.z() + kBg[2] * t_b};
            if (tau.y() > 1.0e-4f) {
                ++compared;
            }
            const size_t base =
                (static_cast<size_t>(py) * kWidth + static_cast<size_t>(px)) * 4;
            for (int c = 0; c < 3; ++c) {
                const float tol = 2.0e-3f + 3.0e-3f * std::fabs(expected[c]);
                EXPECT_NEAR(f.pixels[base + c], expected[c], tol)
                    << "px=" << px << " py=" << py << " channel " << c
                    << " tau=[" << tau.x() << "," << tau.y() << "," << tau.z() << "]";
            }
            // 两次 draw 必须**不改 dst 的 alpha**（靠片元输出 a=1/a=0 实现）：
            // 若哪天有人把加 L 的趟 alpha 写回 1.0，这里会立刻红（alpha 会被累加成 2.0）。
            EXPECT_NEAR(f.pixels[base + 3], 1.0f, 2.0e-3f)
                << "两次 draw 改动了 dst alpha（px=" << px << " py=" << py << "）";
        }
    }
    EXPECT_GT(compared, kWidth * kHeight / 4) << "落进水体的像素太少，用例没有覆盖面";
}

// 介质 + 透明层：层**之前**的介质只能积到层深度（不是整列）。
// 这条直接对上「把水体当成一层（一个代表深度）」那个错法：
// 若介质被当成层，层前后的衰减会一刀切，这个用例就会红。
TEST(WaterMediumGpu, LayerInterleavesWithMedium) {
    Fixture f;
    f.Setup();
    jpov::water::WaterMediumSample m = PureWater();
    // 水面抬到相机之上（y = 10 > 5）⇒ **整条视线都在水里**（相机在水下的分支）。
    // 这样每个像素都满足“层在介质里”（否则 identity VP 下的 z_op 只有 5~6m，
    // 没几个像素能让层落在水面之下，用例覆盖面会接近为零）。
    m.level_y = 10.0f;
    const float alpha = 0.5f;
    const float layer_z = 3.0f;         // 沿视线 3m 处放一层
    jpov::TransparentLayerSet set;
    set.Add(f.Track(MakeLayer(/*cr*/ 1.0f, /*cg*/ 0.5f, /*cb*/ 0.25f, alpha, layer_z)));
    f.Run(set, m, nullptr);

    const jpov::Vec3f ro = {0.0f, 5.0f, 0.0f};
    int compared = 0;
    for (int py = 0; py < kHeight; ++py) {
        for (int px = 0; px < kWidth; ++px) {
            jpov::Vec3f rd;
            float z_op = 0.0f;
            Fixture::RayOf(px, py, ro, &rd, &z_op);
            if (layer_z >= z_op) {
                continue;   // 层在不透明表面之后 ⇒ 合成器会把它夹到 z_op
            }
            jpov::Vec3f tau_l;
            jpov::Vec3f S_l;
            jpov::water::WaterAt(m, ro, rd, layer_z, &tau_l, &S_l);
            jpov::Vec3f tau_op;
            jpov::Vec3f S_op;
            jpov::water::WaterAt(m, ro, rd, z_op, &tau_op, &S_op);
            if (tau_l.y() <= 1.0e-4f) {
                continue;   // 层在水面之上：本用例不覆盖
            }
            ++compared;

            // 合成公式（front-to-back，1 层）：
            //   L = S(z) + exp(−τ(z))·C + (1−α)·(S_op − S(z))
            //   T = (1−α)·exp(−(τ_op − τ(z)))
            const float c[3] = {alpha * 1.0f, alpha * 0.5f, alpha * 0.25f};
            const float layer_t = 1.0f - alpha;
            const float tau_l_arr[3] = {tau_l.x(), tau_l.y(), tau_l.z()};
            const float tau_op_arr[3] = {tau_op.x(), tau_op.y(), tau_op.z()};
            const float S_l_arr[3] = {S_l.x(), S_l.y(), S_l.z()};
            const float S_op_arr[3] = {S_op.x(), S_op.y(), S_op.z()};
            const size_t base =
                (static_cast<size_t>(py) * kWidth + static_cast<size_t>(px)) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                const float L = S_l_arr[ch] + std::exp(-tau_l_arr[ch]) * c[ch] +
                                layer_t * (S_op_arr[ch] - S_l_arr[ch]);
                const float T =
                    layer_t * std::exp(-(tau_op_arr[ch] - tau_l_arr[ch]));
                const float expected = L + kBg[ch] * T;
                const float tol = 3.0e-3f + 4.0e-3f * std::fabs(expected);
                EXPECT_NEAR(f.pixels[base + ch], expected, tol)
                    << "px=" << px << " py=" << py << " channel " << ch;
            }
        }
    }
    EXPECT_GT(compared, kWidth * kHeight / 2);
}

// 无太阳：只剩环境项 ⇒ 仍然与 CPU 一致（且明显比有太阳时暗）。
TEST(WaterMediumGpu, NoSunMatchesCpuAndIsDarker) {
    Fixture f;
    f.Setup();
    jpov::water::WaterMediumSample m = PureWater();
    f.Run(jpov::TransparentLayerSet{}, m, nullptr);
    std::vector<float> with_sun = f.pixels;

    m.has_sun = false;
    f.Run(jpov::TransparentLayerSet{}, m, nullptr);
    const jpov::Vec3f ro = {0.0f, 5.0f, 0.0f};
    for (int py = 0; py < kHeight; ++py) {
        for (int px = 0; px < kWidth; ++px) {
            jpov::Vec3f rd;
            float z_op = 0.0f;
            Fixture::RayOf(px, py, ro, &rd, &z_op);
            jpov::Vec3f tau;
            jpov::Vec3f S;
            jpov::water::WaterAt(m, ro, rd, z_op, &tau, &S);
            const float expected[3] = {S.x() + kBg[0] * std::exp(-tau.x()),
                                       S.y() + kBg[1] * std::exp(-tau.y()),
                                       S.z() + kBg[2] * std::exp(-tau.z())};
            const size_t base =
                (static_cast<size_t>(py) * kWidth + static_cast<size_t>(px)) * 4;
            for (int c = 0; c < 3; ++c) {
                const float tol = 2.0e-3f + 3.0e-3f * std::fabs(expected[c]);
                EXPECT_NEAR(f.pixels[base + c], expected[c], tol);
            }
            if (tau.y() > 1.0e-3f) {
                EXPECT_LT(f.pixels[base], with_sun[base] + 1.0e-4f)
                    << "无太阳时红通道不该更亮（px=" << px << " py=" << py << "）";
            }
        }
    }
}

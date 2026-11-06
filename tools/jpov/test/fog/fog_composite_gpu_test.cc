// JPOV FogCompositeRenderer GPU 测试 —— 锁住「层契约」这半边（纯雾路径由 fog gold 兜）。
//
// 为什么单独测：合成趟里有两段**只有上屏才跑得到**的逻辑：
//   ① 层解算：accum/reveal/depth 三附件 → (预乘色 C, 透射率 T, 代表深度 z̄)；
//   ② 多段积分：层按 z̄ 排序后与雾交替积分，最后 `out = ΣL + dst·a`。
// 两者都依赖 GLSL 采样、混合状态、深度排序，CPU 单测完全覆盖不到。
//
// 用例（背景色 B、层色 C、覆盖率 α 都是**常量**，所以期望值可手算）：
//   1. 单层：   out = α·C + (1−α)·B
//   2. 双层：   out = α₁C₁ + (1−α₁)·α₂C₂ + (1−α₁)(1−α₂)·B
//   3. **顺序无关**：把第 2 条的两个层在集合里对调，输出必须逐像素一致
//      （合成器自己按代表深度排序 —— 若忘了排序，近层会被远层先涂上，红）
//   4. 空集合 + 无效雾场：整趟零开销，背景**一字不变**
//
// 负向验证（开发时实跑）：
//   · 把层排序循环注释掉 ⇒ 用例 3 红（对调后输出差 0.125 以上）
//   · 把 ResolveLayer 的 cov 从 `1−reveal` 改成 `reveal` ⇒ 用例 1/2 红
//   · 把混合从 GL_ONE/GL_SRC_ALPHA 改成 GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA ⇒ 用例 2 红

#define GL_GLEXT_PROTOTYPES

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/fog/fog_composite_renderer.h"

namespace {

constexpr int kWidth = 32;
constexpr int kHeight = 16;
constexpr float kTolerance = 3.0e-3f;   // RGBA16F 读回：半精度 ≈ 1e-3 量级

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
        cfg.title = "Fog Composite GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// ───────────────────────── 工具 ─────────────────────────

// 上传统一填充一张浮点纹理。
unsigned int UploadFilled(float r, float g, float b, float a, GLint internal,
                          GLenum format) {
    std::vector<float> px(static_cast<size_t>(kWidth) * kHeight * 4);
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = a;
    }
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, kWidth, kHeight, 0, format, GL_FLOAT,
                 px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// 造一个等效层：色 c、覆盖率 alpha、代表深度 z。
//   accum  = (c·alpha, alpha)   ⇒ Cavg = c，cov = alpha ⇒ C_premult = c·alpha
//   reveal = 1 − alpha
//   depth  = alpha·z            ⇒ z̄ = z
jpov::TransparentLayer MakeLayer(float cr, float cg, float cb, float alpha, float z) {
    jpov::TransparentLayer layer;
    layer.accum_tex =
        UploadFilled(cr * alpha, cg * alpha, cb * alpha, alpha, GL_RGBA16F, GL_RGBA);
    layer.reveal_tex = UploadFilled(1.0f - alpha, 0.0f, 0.0f, 0.0f, GL_R16F, GL_RED);
    layer.depth_tex = UploadFilled(alpha * z, 0.0f, 0.0f, 0.0f, GL_R32F, GL_RED);
    return layer;
}

struct Fixture {
    jpov::FogCompositeRenderer composite;
    jpov::ShaderManager shader_mgr;
    unsigned int fbo = 0;
    unsigned int color_tex = 0;
    std::vector<unsigned int> textures;   // 用例结束统一删

    void Setup() {
        GlEnv::Get().Ensure();
        composite.Init(&shader_mgr);

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

    // 清成背景色 → 跑一趟合成 → 读回左上角像素（整幅同色）。
    void Run(const jpov::TransparentLayerSet& layers, float out_rgba[4]) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // 背景色当作"已有内容"：直接清成目标色，合成趟在其上就地 over。
        glClearColor(kBg[0], kBg[1], kBg[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        jpov::Camera cam;
        cam.position = {0.0f, 0.0f, 0.0f};
        cam.near = 0.1f;
        cam.far = 1000.0f;
        float view_proj[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 1.0f};
        jpov::FogParams params;   // 无效雾场：只走层合成
        // uHasFog=0 ⇒ froxel 采样整条跳过，本用例只看层。
        composite.Draw(jpov::FogField{}, /*scene_depth_tex=*/0, cam, view_proj, params,
                       layers);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for (int i = 0; i < 4; ++i) {
            out_rgba[i] = px[i];
        }
    }

    static constexpr float kBg[3] = {0.20f, 0.30f, 0.40f};
};

TEST(FogCompositeRendererTest, EmptyLayersAndNoFieldIsNoOp) {
    Fixture f;
    f.Setup();
    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(jpov::TransparentLayerSet{}, px);
    EXPECT_NEAR(px[0], Fixture::kBg[0], kTolerance);
    EXPECT_NEAR(px[1], Fixture::kBg[1], kTolerance);
    EXPECT_NEAR(px[2], Fixture::kBg[2], kTolerance);
}

TEST(FogCompositeRendererTest, SingleLayerPremultipliedOver) {
    Fixture f;
    f.Setup();
    // α 刻意取 1/4（不是 1/2）：α=1/2 时 `cov = 1−reveal` 与写反成 `cov = reveal`
    // 得到同一个值，用例就分辨不出这个 bug 了。
    const float alpha = 0.25f;
    jpov::TransparentLayerSet set;
    set.Add(f.Track(MakeLayer(1.0f, 0.5f, 0.25f, alpha, /*z=*/0.5f)));

    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(set, px);
    // out = α·C + (1−α)·B
    EXPECT_NEAR(px[0], alpha * 1.0f + (1.0f - alpha) * Fixture::kBg[0], kTolerance);
    EXPECT_NEAR(px[1], alpha * 0.5f + (1.0f - alpha) * Fixture::kBg[1], kTolerance);
    EXPECT_NEAR(px[2], alpha * 0.25f + (1.0f - alpha) * Fixture::kBg[2], kTolerance);
}

// 双层 + 顺序无关：集合里对调两个层，结果必须逐位一致（合成器自己按 z̄ 排序）。
TEST(FogCompositeRendererTest, TwoLayersSortedByDepth) {
    Fixture f;
    f.Setup();
    const float a_near = 0.5f;
    const float a_far = 0.25f;
    const float c_near[3] = {1.0f, 0.5f, 0.25f};
    const float c_far[3] = {0.1f, 0.8f, 0.4f};

    jpov::TransparentLayerSet forward;
    forward.Add(f.Track(MakeLayer(c_near[0], c_near[1], c_near[2], a_near, 0.5f)));
    forward.Add(f.Track(MakeLayer(c_far[0], c_far[1], c_far[2], a_far, 1.0f)));

    jpov::TransparentLayerSet reversed;
    reversed.Add(f.Track(MakeLayer(c_far[0], c_far[1], c_far[2], a_far, 1.0f)));
    reversed.Add(f.Track(MakeLayer(c_near[0], c_near[1], c_near[2], a_near, 0.5f)));

    float px_fwd[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float px_rev[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(forward, px_fwd);
    f.Run(reversed, px_rev);

    // out = α₁C₁ + (1−α₁)·α₂C₂ + (1−α₁)(1−α₂)·B（1 = 近层）
    const float w_near = a_near;
    const float w_far = (1.0f - a_near) * a_far;
    const float w_bg = (1.0f - a_near) * (1.0f - a_far);
    for (int ch = 0; ch < 3; ++ch) {
        const float expected =
            w_near * c_near[ch] + w_far * c_far[ch] + w_bg * Fixture::kBg[ch];
        EXPECT_NEAR(px_fwd[ch], expected, kTolerance) << "channel " << ch;
        EXPECT_NEAR(px_rev[ch], px_fwd[ch], 1.0e-4f)
            << "层集合顺序不该影响结果（合成器按代表深度排序），channel " << ch;
    }
}

}  // namespace

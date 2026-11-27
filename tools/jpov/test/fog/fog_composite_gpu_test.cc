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
#include "tools/jpov/src/water/water_medium_math.h"

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

// 加法集附件：存的是**已预乘**的 Σ(色·α)，与生产端的 `FragAdd` 一致。
jpov::TransparentLayer WithAdditive(const jpov::TransparentLayer& layer, float ar,
                                    float ag, float ab) {
    jpov::TransparentLayer out = layer;
    out.additive_tex = UploadFilled(ar, ag, ab, 0.0f, GL_RGBA16F, GL_RGBA);
    return out;
}

struct Fixture {
    jpov::FogCompositeRenderer composite;
    jpov::ShaderManager shader_mgr;
    unsigned int fbo = 0;
    unsigned int color_tex = 0;
    // 本 fixture 的相机矩阵（默认恒等：层/雾用例不需要真实投影；
    // 需要真实的 z_op 时由用例设成透视矩阵）。
    float view_proj[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<unsigned int> textures;   // 用例结束统一删

    // medium_glsl = 非空 ⇒ 换一份**合成用**的介质提供者（物理被换成可手算的解析式，
    // 这样「层按自己的深度被衰减」这条不变式才能用常数手算验证）。
    void Setup(const char* medium_glsl = nullptr) {
        GlEnv::Get().Ensure();
        composite.Init(&shader_mgr, medium_glsl);

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
        if (layer.additive_tex != 0) {
            textures.push_back(layer.additive_tex);
        }
        return layer;
    }

    // 清成背景色 → 跑一趟合成 → 读回左上角像素（整幅同色）。
    void Run(const jpov::TransparentLayerSet& layers, float out_rgba[4],
             const jpov::water::WaterMediumSample* medium = nullptr,
             unsigned int scene_depth_tex = 0, int read_x = 0, int read_y = 0) {
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
        jpov::FogParams params;   // 无效雾场：只走层合成
        // uHasFog=0 ⇒ froxel 采样整条跳过，本用例只看层。
        composite.Draw(jpov::FogField{}, scene_depth_tex, cam, view_proj, params, layers,
                       medium);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glReadPixels(read_x, read_y, 1, 1, GL_RGBA, GL_FLOAT, px);
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


// ───────────────── 层必须按**自己的深度**被介质衰减（2026-11-27 回归）─────────────────
//
// 这是「条带入水即消失」的那个 bug 的最小复现：介质合成趟输出 `out = L + dst·T`，
// 而 `T` 只用**不透明场景深度**算。层如果不在 `L` 里（= 自己先 over 进 dst），
// 就会被 `T` 乘掉 —— 开阔水体里 `z_op → ∞` ⇒ `T → 0` ⇒ 整层被擦成水体注入色。
//
// 用一份**可手算**的合成用介质：τ(z) = 0.1·z（每米 0.1，灰），S ≡ 0。
// 于是「层 z=6.7 m、α=1、色 C」的期望值是 exp(−0.67)·C，而背景（无穷远）为 0。
//
// 负向验证（开发时实跑）：把层改回「先 over 进 dst 再跑介质」⇒ 读回 0（用例红）。
constexpr const char* kLinearAbsorbMedium = R"glsl(
void MediumAt(vec3 ro, vec3 rd, float z, out vec3 tau, out vec3 S) {
    tau = vec3(0.1) * z;   // 每米 0.1 的光学厚度（灰，与方向无关）
    S = vec3(0.0);
}
)glsl";

TEST(FogCompositeRendererTest, LayerIsAttenuatedByItsOwnDepthNotTheBackground) {
    Fixture f;
    f.Setup(kLinearAbsorbMedium);
    jpov::water::WaterMediumSample medium;   // 数值被 kLinearAbsorbMedium 顶掉，只为
                                             // 触发「逐通道透射」两趟路径
    const float c[3] = {1.0f, 0.5f, 0.25f};
    const float z_layer = 6.7f;
    jpov::TransparentLayerSet set;
    set.Add(f.Track(MakeLayer(c[0], c[1], c[2], /*alpha=*/1.0f, z_layer)));

    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(set, px, &medium);

    const float transmit = std::exp(-0.1f * z_layer);
    for (int ch = 0; ch < 3; ++ch) {
        // 背景（无穷远）本该被介质吃干（T→0），层的贡献只吃自己那一段 τ。
        EXPECT_NEAR(px[ch], transmit * c[ch], kTolerance) << "channel " << ch;
    }
    // 非恒真断言：结果必须**明显不等于 0**（= 被当成背景吃掉的旧行为）。
    EXPECT_GT(px[0], 0.4f) << "层被介质吃掉了 ⇒ 说明它没进 L（回到了旧路径）";
}

// 多层：每一层各吃自己那一段 τ（近层不被远层的 τ 影响，远层被近层的 W 衰减）。
TEST(FogCompositeRendererTest, EachLayerIsAttenuatedAtItsOwnDepth) {
    Fixture f;
    f.Setup(kLinearAbsorbMedium);
    jpov::water::WaterMediumSample medium;
    const float c_near[3] = {1.0f, 0.0f, 0.0f};
    const float c_far[3] = {0.0f, 0.0f, 1.0f};
    const float z_near = 2.0f;
    const float z_far = 8.0f;
    const float a_near = 0.5f;
    const float a_far = 0.5f;

    jpov::TransparentLayerSet set;
    // 故意先加远层：合成器必须按代表深度重排。
    set.Add(f.Track(MakeLayer(c_far[0], c_far[1], c_far[2], a_far, z_far)));
    set.Add(f.Track(MakeLayer(c_near[0], c_near[1], c_near[2], a_near, z_near)));

    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(set, px, &medium);

    // L = exp(−τ(z1))·(α1·C1) + (1−α1)·exp(−τ(z2))·(α2·C2)；背景无穷远 ⟹ T=0。
    const float e1 = std::exp(-0.1f * z_near);
    const float e2 = std::exp(-0.1f * z_far);
    EXPECT_NEAR(px[0], e1 * a_near * c_near[0], kTolerance);
    EXPECT_NEAR(px[1], e1 * a_near * c_near[1], kTolerance);
    EXPECT_NEAR(px[2], (1.0f - a_near) * e2 * a_far * c_far[2], kTolerance);
}

// ─────────── 覆盖率必须同时作用在「层」和「背景」两项上（2026-11-27）───────────
//
// 这是「条带变实心」那个 bug 的最小复现：合成结果是
//     out = ΣL + dst·T，其中 L 含 α·C、T 含 (1−α)
// 如果解析把覆盖率丢了（cov 恒 1 / 只有层没有背景项），半透明条带就会变成实心片。
//
// 与上一个用例的区别：这里给一个**有限的背景深度**（不透明几何在 10 m 处），
// 于是 `T` 与 `L` 两项都能用常数手算 —— 只查一项的话「cov=1」这类错误会被
// 「两项都错但比值对」蒙过去。
//
// 负向验证（开发时实跑）：把 ResolveLayer 的 cov 改成常数 1.0 ⇒ 本用例红。
constexpr float kBgDepthM = 10.0f;

// 标准 GL 透视（fov 很小 ⇒ 整个 32×16 视口的视线都≈平行于 −z，z_op ≈ 常数）。
void MakeNarrowPerspective(float fov_deg, float aspect, float znear, float zfar,
                           float out[16]) {
    const float f = 1.0f / std::tan(fov_deg * 0.5f * 3.14159265358979323846f / 180.0f);
    for (int i = 0; i < 16; ++i) {
        out[i] = 0.0f;
    }
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (zfar + znear) / (znear - zfar);
    out[11] = -1.0f;
    out[14] = 2.0f * zfar * znear / (znear - zfar);
}

// 与 fog_composite 的 ZView 同一口径：视空间 z（米）→ 窗口深度 [0,1]。
float WindowDepthFor(float z_m, float znear, float zfar) {
    const float ndc = ((zfar + znear) - 2.0f * znear * zfar / z_m) / (zfar - znear);
    return ndc * 0.5f + 0.5f;
}

TEST(FogCompositeRendererTest, CoverageShapesBothLayerAndBackgroundTerms) {
    Fixture f;
    f.Setup(kLinearAbsorbMedium);
    jpov::water::WaterMediumSample medium;

    const float znear = 0.1f;
    const float zfar = 1000.0f;
    const float c[3] = {1.0f, 0.5f, 0.25f};
    const float alpha = 0.4f;
    const float z_layer = 4.0f;

    // 背景深度：不透明几何在 kBgDepthM 处（整屏同值，视口很窄 ⇒ 逐像素 z_op≈常数）。
    std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight,
                             WindowDepthFor(kBgDepthM, znear, zfar));
    unsigned int depth_tex = 0;
    glGenTextures(1, &depth_tex);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kWidth, kHeight, 0, GL_RED, GL_FLOAT,
                 depth.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // 真实透视：z_op 才等于「不透明几何在 10 m」而不是恒等矩阵下的 0.98 m。
    MakeNarrowPerspective(/*fov_deg=*/10.0f,
                          static_cast<float>(kWidth) / static_cast<float>(kHeight),
                          znear, zfar, f.view_proj);

    jpov::TransparentLayerSet set;
    set.Add(f.Track(MakeLayer(c[0], c[1], c[2], alpha, z_layer)));

    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(set, px, &medium, depth_tex, /*read_x=*/kWidth / 2, /*read_y=*/kHeight / 2);
    glDeleteTextures(1, &depth_tex);

    // out = dst·(1−α)·exp(−τ(z_op)) + α·C·exp(−τ(z_layer))，τ(z) = 0.1z
    const float t_layer = std::exp(-0.1f * z_layer);
    const float t_bg = std::exp(-0.1f * kBgDepthM);
    for (int ch = 0; ch < 3; ++ch) {
        const float expected =
            alpha * c[ch] * t_layer + (1.0f - alpha) * Fixture::kBg[ch] * t_bg;
        EXPECT_NEAR(px[ch], expected, kTolerance) << "channel " << ch;
    }
    // 非恒真断言：背景项必须真的在（cov=1 时它会整个消失），且不能整块被层盖住。
    const float bg_term = (1.0f - alpha) * Fixture::kBg[1] * t_bg;
    EXPECT_GT(px[1], alpha * c[1] * t_layer) << "背景项被吃掉了 ⇒ 覆盖率没有作用在 T 上";
    EXPECT_GT(bg_term, 1.0e-3f);   // 自检：这条期望本身不能接近 0（否则断言恒真）
}

// 加法集：只有加法集的层（无覆盖集）也必须把光加进画面（α≡0 不遮挡背景）。
// 无介质 ⇒ 恒等路径，期望 out = 背景 + 加法集。
TEST(FogCompositeRendererTest, AdditiveOnlyLayerAddsLight) {
    Fixture f;
    f.Setup();
    const float add[3] = {0.40f, 0.20f, 0.10f};
    jpov::TransparentLayerSet set;
    set.Add(f.Track(WithAdditive(MakeLayer(0.9f, 0.9f, 0.9f, /*alpha=*/0.0f, 1.0f),
                                 add[0], add[1], add[2])));

    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f.Run(set, px);

    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(px[ch], Fixture::kBg[ch] + add[ch], kTolerance) << "channel " << ch;
    }
}

}  // namespace

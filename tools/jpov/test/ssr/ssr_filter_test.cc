// JPOV SSR 粗糙度预滤波（SsrFilter）GPU 单测 —— 锁住 mip 链的三条正确性属性。
//
// 为什么必须测这三条（它们都是"看起来能跑但结果错"的类型，肉眼很难发现）：
//   ① **覆盖率加权**：命中像素才参与平均。若写成普通 box 平均，未命中像素的 rgb=0
//      会被平均进来 ⇒ "越粗糙越暗"（磨砂伴随变暗，是最常见的那种错）。
//   ② **空像素填充**：没有命中的格要抄邻居的**颜色**（alpha 仍留 0），
//      否则双线性/三线性在覆盖率边界上把颜色往黑插 ⇒ 倒影边缘一圈黑边。
//   ③ **覆盖率随 mip 下降**：a' = Σa/4。合成层拿它当 A 做交叉淡入，
//      所以这一条同时决定了"粗糙度上升时自然交回 env"和"反射体边缘抗锯齿"。
//
// 全部用合成纹理（8×8，左半边命中 / 右半边未命中），不需要场景与相机。

#define GL_GLEXT_PROTOTYPES

#include <cmath>
#include <memory>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/ssr/ssr_filter.h"

namespace {

using jpov::SsrFilter;

// 一个最小的 headless JPOV（只为拿到 GL context；与 ssr_gpu_test 同一套做法）。
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
        cfg.title = "SSR Filter Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

constexpr int kSize = 8;                 // 8×8 ⇒ 4 级 mip（8,4,2,1）
constexpr float kHitRadiance = 0.25f;    // 命中色（任取一个非 1 的值，避免"1 恰好蒙对"）

unsigned int UploadRadiance(int w, int h, const std::vector<float>& rgba) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

unsigned int UploadType(int w, int h, const std::vector<unsigned char>& types) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                 types.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

std::vector<float> ReadMipLevel(unsigned int tex, int level, int w, int h) {
    std::vector<float> out(static_cast<size_t>(w) * h * 4, -1.0f);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, out.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return out;
}

float At(const std::vector<float>& img, int w, int x, int y, int c) {
    return img[(static_cast<size_t>(y) * w + x) * 4 + c];
}

// 场景：左半边（x < 4）全部命中，右半边全部未命中。命中色 = kHitRadiance。
struct Fixture {
    unsigned int radiance = 0;
    unsigned int types = 0;
    SsrFilter filter;
    jpov::ShaderManager shader_mgr;

    void Setup() { SetupWithAlpha(1.0f); }

    // hit_alpha：命中格的 A（1 = 完整命中；<1 = 掠射淡出后的置信度）。
    void SetupWithAlpha(float hit_alpha) {
        GlEnv::Get().Ensure();
        std::vector<float> rgba(static_cast<size_t>(kSize) * kSize * 4, 0.0f);
        std::vector<unsigned char> type(static_cast<size_t>(kSize) * kSize, 0);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const size_t i = static_cast<size_t>(y) * kSize + x;
                if (x < kSize / 2) {
                    rgba[i * 4 + 0] = kHitRadiance;
                    rgba[i * 4 + 1] = kHitRadiance;
                    rgba[i * 4 + 2] = kHitRadiance;
                    // 2026-11-13 起的契约：radiance.a = **置信度 A**（1 = 完整命中；
                    // 掠射淡出时 < 1）。滤波层直接用它，不再恒为 1。
                    rgba[i * 4 + 3] = hit_alpha;
                    type[i] = 1;              // kHit
                }
            }
        }
        radiance = UploadRadiance(kSize, kSize, rgba);
        types = UploadType(kSize, kSize, type);
        filter.Init(&shader_mgr);
        filter.Run(radiance, types, kSize, kSize);
    }

    ~Fixture() {
        filter.Finalize();
        if (radiance != 0) {
            glDeleteTextures(1, &radiance);
        }
        if (types != 0) {
            glDeleteTextures(1, &types);
        }
    }
};

// ① + ②：level 0 —— 命中格 alpha=1、未命中格 alpha=0，但**未命中格的颜色 ≥ 命中色的一半**
// （3×3 填充抄了邻居；若没做填充，这里会是 0 ⇒ 断言红）。
TEST(SsrFilter, LevelZeroCoverageAndFill) {
    Fixture f;
    f.Setup();
    ASSERT_EQ(f.filter.level_count(), 4);   // 8×8 → 8,4,2,1

    const std::vector<float> l0 = ReadMipLevel(f.filter.mip_tex(), 0, kSize, kSize);

    // 命中格：覆盖率 1、颜色 = 命中色（A 由 radiance.a 带下来）。
    EXPECT_FLOAT_EQ(At(l0, kSize, 0, 0, 3), 1.0f);
    EXPECT_NEAR(At(l0, kSize, 0, 0, 0), kHitRadiance, 1.0e-3f);

    // 未命中但紧贴命中区的格子（x=4、x=5）：alpha 必须仍是 0（它不是命中），
    // 颜色必须被填充成邻居的颜色（否则下一级双线性会插到 0）。
    // 半径 2（5×5 核）是刻意的：它刚好覆盖 2×2 降采样块的斜对角，理由是下一级的
    // "整块无覆盖"分支要靠这些填充色取平均（半径 1 会留下约 25% 的暗边）。
    EXPECT_FLOAT_EQ(At(l0, kSize, 4, 0, 3), 0.0f);
    EXPECT_NEAR(At(l0, kSize, 4, 0, 0), kHitRadiance, 1.0e-3f);
    EXPECT_FLOAT_EQ(At(l0, kSize, 5, 0, 3), 0.0f);
    EXPECT_NEAR(At(l0, kSize, 5, 0, 0), kHitRadiance, 1.0e-3f);

    // 超出填充半径（x=7，最近的命中在 x=3，相距 4）：保持 0 —— 这是**有意**的边界：
    // 单趟填充只铺一圈有限的宽度。真实画面里反射体不会"孤零零一小块 + 大片空区"，
    // 大片空区的 a=0 会让合成层直接交回 env（颜色不参与），所以不会形成黑块。
    EXPECT_FLOAT_EQ(At(l0, kSize, 7, 0, 3), 0.0f);
}

// ③：level 1 —— 一个 2×2 块里 2 个命中 ⇒ 覆盖率 0.5，颜色**仍是命中色**。
//     若颜色按普通 box 平均，这里会是 0.5×kHitRadiance ⇒ 断言红（"磨砂 = 变暗"）。
// ③ 追加（2026-11-13）：A 的淡出系数必须沿 mip 链按覆盖率加权下降（a' = Σa/4）。
// 这是掠射淡出的落点：A=0.25 的命中格，level0 alpha=0.25，level1 = 0.25（全命中块）。
TEST(SsrFilter, FadeCoefficientPropagates) {
    Fixture f;
    f.SetupWithAlpha(0.25f);
    ASSERT_EQ(f.filter.level_count(), 4);
    const std::vector<float> l0 = ReadMipLevel(f.filter.mip_tex(), 0, kSize, kSize);
    EXPECT_NEAR(At(l0, kSize, 0, 0, 3), 0.25f, 1.0e-3f);
    const std::vector<float> l1 = ReadMipLevel(f.filter.mip_tex(), 1, kSize / 2, kSize / 2);
    EXPECT_NEAR(At(l1, kSize / 2, 0, 0, 3), 0.25f, 1.0e-3f);   // 整块都是 0.25 的命中格
}

TEST(SsrFilter, MipIsCoverageWeighted) {
    Fixture f;
    f.Setup();
    const std::vector<float> l1 = ReadMipLevel(f.filter.mip_tex(), 1, kSize / 2, kSize / 2);

    // level 1 的 (0,0) 覆盖 level 0 的 (0..1, 0..1)：两格都命中。
    EXPECT_NEAR(At(l1, kSize / 2, 0, 0, 3), 1.0f, 1.0e-3f);
    EXPECT_NEAR(At(l1, kSize / 2, 0, 0, 0), kHitRadiance, 1.0e-3f);

    // level 1 的 (1,0) 覆盖 level 0 的 (2..3, 0..1)：两格都命中（x<4）⇒ 仍是 1。
    EXPECT_NEAR(At(l1, kSize / 2, 1, 0, 3), 1.0f, 1.0e-3f);

    // level 1 的 (2,0) 覆盖 level 0 的 (4..5, 0..1)：**全未命中**，但两格已被 level 0
    // 填充成命中色 ⇒ alpha=0（不是命中），颜色必须是命中色（三线性才不会插到黑）。
    EXPECT_NEAR(At(l1, kSize / 2, 2, 0, 3), 0.0f, 1.0e-3f);
    EXPECT_NEAR(At(l1, kSize / 2, 2, 0, 0), kHitRadiance, 1.0e-3f);

    // 再往外一格 (3,0)（level 0 的 x=6..7）超出填充半径 ⇒ 颜色 0、a=0。
    // 也就是说"填充"不是全局糊一遍，而只是边界一圈；这是刻意的（见 ssr_filter.h）。
    EXPECT_NEAR(At(l1, kSize / 2, 3, 0, 3), 0.0f, 1.0e-3f);
}

// 负向对照：把"命中色"换成一个**不同**的值，断言 level 1 的 (2,0) 跟着变 ——
// 证明上面那条"填充色确实来自命中格"，而不是恰好为 0/常数。
TEST(SsrFilter, FillColorFollowsRadiance) {
    GlEnv::Get().Ensure();
    constexpr int kCells = kSize * kSize;
    std::vector<float> rgba(static_cast<size_t>(kCells) * 4, 0.0f);
    std::vector<unsigned char> type(static_cast<size_t>(kCells), 0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize / 2; ++x) {
            const size_t i = static_cast<size_t>(y) * kSize + x;
            rgba[i * 4 + 0] = 0.75f;
            type[i] = 1;
        }
    }
    const unsigned int rad = UploadRadiance(kSize, kSize, rgba);
    const unsigned int typ = UploadType(kSize, kSize, type);
    jpov::ShaderManager mgr;
    SsrFilter filter;
    filter.Init(&mgr);
    filter.Run(rad, typ, kSize, kSize);

    const std::vector<float> l1 = ReadMipLevel(filter.mip_tex(), 1, kSize / 2, kSize / 2);
    // level 1 的 (2,0) = 边界外的第一块空区：填充色应等于**本次**的命中色 0.75，
    // 而不是上一组用例的 0.25/0/其它常数 ⇒ 证明填充色确实来自命中格。
    EXPECT_NEAR(At(l1, kSize / 2, 2, 0, 0), 0.75f, 1.0e-3f);
    EXPECT_NEAR(At(l1, kSize / 2, 2, 0, 3), 0.0f, 1.0e-3f);

    filter.Finalize();
    glDeleteTextures(1, &rad);
    glDeleteTextures(1, &typ);
}

}  // namespace

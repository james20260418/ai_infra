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

// ───────────── 遮丑滤波器（2026-11-13）：三条独立可关，默认零回归 ─────────────
//
// 这一组测的是"开关的语义"，不是画质：关 = 逐字节旧行为；每条打开后**只**动它该动的地方。

unsigned int UploadFloatTex(int w, int h, int internal, int format,
                            const std::vector<float>& data) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// 8×8 合成输入 → 跑一遍 → 读回 mip level 0（= 遮丑链的最终输出）。
// use_guides = false 走 4 参数重载（= 2026-11-13 之前的行为）。
std::vector<float> RunPost(const std::vector<float>& rgba, bool use_guides,
                           const jpov::SsrFilterConfig& cfg) {
    GlEnv::Get().Ensure();
    const int n = kSize * kSize;
    std::vector<unsigned char> type(static_cast<size_t>(n), 0);
    std::vector<float> wbuf(static_cast<size_t>(n), 0.05f);   // 全部同一平面
    std::vector<float> nrm(static_cast<size_t>(n) * 4, 0.0f);
    for (int i = 0; i < n; ++i) {
        if (rgba[static_cast<size_t>(i) * 4 + 3] > 0.0f) {
            type[static_cast<size_t>(i)] = 1;                 // kHit
        }
        nrm[static_cast<size_t>(i) * 4 + 2] = 1.0f;           // 法线 = +Z（同面）
        nrm[static_cast<size_t>(i) * 4 + 3] = 1.0f;
    }
    const unsigned int rad = UploadRadiance(kSize, kSize, rgba);
    const unsigned int typ = UploadType(kSize, kSize, type);
    const unsigned int wt = UploadFloatTex(kSize, kSize, GL_R32F, GL_RED, wbuf);
    const unsigned int nt = UploadFloatTex(kSize, kSize, GL_RGBA16F, GL_RGBA, nrm);

    jpov::ShaderManager mgr;
    SsrFilter filter;
    filter.Init(&mgr);
    if (use_guides) {
        jpov::SsrFilterGuides g;
        g.down_w_tex = wt;
        g.down_normal_tex = nt;
        filter.Run(rad, typ, kSize, kSize, g, cfg);
    } else {
        filter.Run(rad, typ, kSize, kSize);
    }
    std::vector<float> out = ReadMipLevel(filter.mip_tex(), 0, kSize, kSize);
    filter.Finalize();
    glDeleteTextures(1, &rad);
    glDeleteTextures(1, &typ);
    glDeleteTextures(1, &wt);
    glDeleteTextures(1, &nt);
    return out;
}

std::vector<float> SolidGrid(float a) {
    std::vector<float> rgba(static_cast<size_t>(kSize) * kSize * 4, 0.0f);
    for (int i = 0; i < kSize * kSize; ++i) {
        rgba[static_cast<size_t>(i) * 4 + 3] = a;
    }
    return rgba;
}

void SetPx(std::vector<float>* rgba, int x, int y, float rgb, float a) {
    const size_t i = static_cast<size_t>(y) * kSize + x;
    (*rgba)[i * 4 + 0] = rgb;
    (*rgba)[i * 4 + 1] = rgb;
    (*rgba)[i * 4 + 2] = rgb;
    (*rgba)[i * 4 + 3] = a;
}

// 默认零回归：三条全关时，给不给引导都必须与旧路径**逐字节**相同。
TEST(SsrFilterPost, DisabledIsByteIdentical) {
    std::vector<float> rgba = SolidGrid(0.0f);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize / 2; ++x) {
            SetPx(&rgba, x, y, kHitRadiance, 1.0f);
        }
    }
    const jpov::SsrFilterConfig off;
    const std::vector<float> legacy = RunPost(rgba, /*use_guides=*/false, off);
    const std::vector<float> with_guides = RunPost(rgba, /*use_guides=*/true, off);
    ASSERT_EQ(legacy.size(), with_guides.size());
    for (size_t i = 0; i < legacy.size(); ++i) {
        EXPECT_FLOAT_EQ(legacy[i], with_guides[i]) << "第 " << i << " 个分量不同";
    }
}

// ① 补洞：只补"两侧都命中"的 1px 缝；单侧命中的边界与宽洞都不许动。
TEST(SsrFilterPost, HoleFillOnlyClosesTwoSidedGap) {
    std::vector<float> rgba = SolidGrid(0.0f);
    // 第 4 行：x=2 命中、(3) 空、x=4 命中 ⇒ 3 是"两侧都命中"的缝。
    SetPx(&rgba, 2, 4, 0.5f, 1.0f);
    SetPx(&rgba, 4, 4, 0.5f, 1.0f);
    // 第 6 行：x=2 命中、x=3 空、x=4 也空 ⇒ 3 只有单侧命中，不许填。
    SetPx(&rgba, 2, 6, 0.5f, 1.0f);

    jpov::SsrFilterConfig cfg;
    cfg.hole_fill = true;
    const std::vector<float> out = RunPost(rgba, /*use_guides=*/true, cfg);

    // 缝被补上：覆盖率回到 1，颜色取两侧命中的平均（0.5）。
    EXPECT_NEAR(At(out, kSize, 3, 4, 3), 1.0f, 1.0e-3f);
    EXPECT_NEAR(At(out, kSize, 3, 4, 0), 0.5f, 1.0e-3f);
    // 命中格原样（不是"整片糊一遍"）。
    EXPECT_NEAR(At(out, kSize, 2, 4, 3), 1.0f, 1.0e-3f);
    // 负向：单侧命中的空位保持 0（若把判据写成"有任意命中邻居就填"，这里会变 1）。
    EXPECT_FLOAT_EQ(At(out, kSize, 3, 6, 3), 0.0f);
    // 远离任何命中的空位保持 0。
    EXPECT_FLOAT_EQ(At(out, kSize, 7, 0, 3), 0.0f);
}

// ③ à-trous：单个孤立命中会被摊开 —— 中心覆盖率 = 36/256（B3 核的中心权 × 无边缘拒绝），
//    而核覆盖不到的角落必须仍是 0。负向：不摊开（仍 1）或摊成一片（角落 > 0）都会红。
TEST(SsrFilterPost, AtrousSpreadsIsolatedSample) {
    std::vector<float> rgba = SolidGrid(0.0f);
    SetPx(&rgba, 4, 4, 0.5f, 1.0f);   // 唯一命中，5×5 窗口正好完整落在 8×8 内

    jpov::SsrFilterConfig cfg;
    cfg.atrous = true;
    cfg.atrous_iterations = 1;
    const std::vector<float> out = RunPost(rgba, /*use_guides=*/true, cfg);

    const float center = 36.0f / 256.0f;   // (6·6)/16²：B3 核中心项
    EXPECT_NEAR(At(out, kSize, 4, 4, 3), center, 2.0e-3f);
    // 颜色不因 A 变小而丢（按 A 加权的平均值仍是原色）。
    EXPECT_NEAR(At(out, kSize, 4, 4, 0), 0.5f, 1.0e-3f);
    // 核外的角落保持 0（不是全屏糊）。
    EXPECT_FLOAT_EQ(At(out, kSize, 0, 0, 3), 0.0f);
}

// ② 离群抑制：3×3 里 8 个 0.25 + 中心 1.0 ⇒ 中心被夹回 mean + k·σ，正常样本不动。
TEST(SsrFilterPost, OutlierClampPullsOnlyTheOutlier) {
    std::vector<float> rgba = SolidGrid(0.0f);
    for (int y = 3; y <= 5; ++y) {
        for (int x = 3; x <= 5; ++x) {
            SetPx(&rgba, x, y, 0.25f, 1.0f);
        }
    }
    SetPx(&rgba, 4, 4, 1.0f, 1.0f);   // 离群点

    jpov::SsrFilterConfig cfg;
    cfg.outlier_clamp = true;
    cfg.clamp_sigma = 2.0f;
    const std::vector<float> out = RunPost(rgba, /*use_guides=*/true, cfg);

    const float mean = (8.0f * 0.25f + 1.0f) / 9.0f;
    const float var = (8.0f * (0.25f - mean) * (0.25f - mean) + (1.0f - mean) * (1.0f - mean)) / 9.0f;
    const float expect_center = mean + 2.0f * std::sqrt(var);
    // 负向：不做 clamp 时这里是 1.0（这条断言就是"确实动了"）。
    EXPECT_NEAR(At(out, kSize, 4, 4, 0), expect_center, 3.0e-3f);
    EXPECT_LT(At(out, kSize, 4, 4, 0), 0.95f);
    // 正常样本必须保持原值（只夹离群点，不整体模糊）。
    EXPECT_NEAR(At(out, kSize, 3, 3, 0), 0.25f, 3.0e-3f);
    // 覆盖率不参与 clamp（A 原样）。
    EXPECT_NEAR(At(out, kSize, 4, 4, 3), 1.0f, 1.0e-3f);
}

}  // namespace

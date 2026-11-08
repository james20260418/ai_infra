// JPOV SSR GPU 像素测试 —— 锁住"CPU 单测验不到的那半边"（着色器 / 纹理绑定 / tile 映射）
//
// 三板斧：
//   ① **CPU 参考实现对拍**（最强的那个）：同一张合成深度图分别喂给 CPU 的 MarchSlabs 和
//      GPU 的两趟 pass，逐像素断言「命中/未命中一致 + 命中格一致 + s 相差 < 0.05 px」。
//      着色器里任何一处与 ssr_math.h 走样（1/d 插值、tile 映射、tile 中心 uv、uniform 传错）
//      都会在这里红 —— 不需要维护 png gold，也不怕 llvmpipe 精度漂移。
//   ② 手推的定性结论（不依赖 CPU 实现）：均匀深度墙 + 地板法线 ⇒ **下半个屏幕命中、
//      上半个屏幕出屏**（射线往屏幕上方且远离相机飞），命中处 s 只有几个像素（柱子只有
//      T=0.2 m 厚）。方向约定若写反，这个模式会整个翻过来。
//   ③ tile 等价性：tile=1/2/4/8 在同一 max_ray_pixels 下 radiance / hit_s **逐字节相同**
//      —— 这才是"tile 只是性能旋钮"的证据（否则它就在偷偷改画面）。
//
// 合成输入，不需要场景 / 物体 / 相机矩阵：SSR 的输入本来就只有三张纹理。
//
// 负向验证（2026-10-29 实跑，证明断言有牙）：
//   · 把着色器的 SsrWAt 改成 d 空间插值 ⇒ ① 的 |Δs| 断言红（s 差几十像素）
//   · 把 tile 中心 uv 写成 gl_FragCoord/uTile/uRes ⇒ ① 红（整幅偏 0.25 px，命中格成片错）
//   · 把 march 的 uTile 传成 1（映射错位）⇒ ① 红、③ 红

#define GL_GLEXT_PROTOTYPES

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/ssr/ssr_math.h"
#include "tools/jpov/src/ssr/ssr_renderer.h"

namespace {

using jpov::ssr::BuildScreenRay;
using jpov::ssr::ClipRayLength;
using jpov::ssr::DeriveParams;
using jpov::ssr::DerivedParams;
using jpov::ssr::kNoHit;
using jpov::ssr::HasDepthData;
using jpov::ssr::MarchSlabs;
using jpov::ssr::OffsetRayOrigin;
using jpov::ssr::ReconstructNormal;
using jpov::ssr::ReflectDir;
using jpov::ssr::ScreenRay;
using jpov::SsrInputs;
using jpov::SsrOutputs;
using jpov::SsrRenderer;
using jpov::SsrResult;
using jpov::ssr::Vec3;
using jpov::ssr::ViewPointAtUv;

constexpr int kWidth = 96;
constexpr int kHeight = 64;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;
constexpr float kTanHalf = 0.57735027f;   // 60° 垂直 FOV
constexpr float kAspect = 1.5f;           // 96:64

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
        cfg.title = "SSR GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// ───────────────────────── 合成输入 / 读回 ─────────────────────────

struct Scene {
    std::vector<float> window_depth;   // R32F：gl_FragCoord.z
    std::vector<float> normal_xy;      // RG16F：视空间法线 .xy
    std::vector<float> color;          // RGBA16F：R=x/W, G=y/H, B=0.25（按格可反查）
    // 可选：非空 ⇒ 法线附件改成 RGBA32F，.z = 1 表示该 texel 有法线数据（朝向判据用）。
    // 空 = 整张 .z = 0 ⇒ 朝向判据不生效（与加判据之前的行为一致）。
    std::vector<float> attr_z;
    // 可选：测试自用的"这个像素属于哪个面"标记（**不上传**，只用于圈定断言范围）。
    //   0=天空 1=地板（反射体） 2=桌子顶面（法线朝上） 3=墙（法线朝相机）
    std::vector<int> kind;
};

// 均匀深度 + 地板法线（ny=0.95 ⇒ N 几乎贴着屏幕法向，反射射线几乎贴着地面飞）。
// 后半张图（y ≥ h/2）深度更远 ⇒ 射线能多飞一段才命中，覆盖长路径。
Scene MakeScene(bool two_layers, float ny = 0.95f, int sky_rows = 0) {
    Scene sc;
    sc.window_depth.resize(static_cast<size_t>(kWidth) * kHeight);
    sc.normal_xy.resize(static_cast<size_t>(kWidth) * kHeight * 2);
    sc.color.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    auto window_depth_of = [](float d) {
        // ndc = (far + near − 2·near·far/d) / (far − near)
        const float ndc =
            ((kFar + kNear) - 2.0f * kNear * kFar / d) / (kFar - kNear);
        return 0.5f * ndc + 0.5f;
    };
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            if (sky_rows > 0 && y >= kHeight - sky_rows) {
                sc.window_depth[i] = 1.0f;   // 天空：无深度数据（清空值）
            } else {
                const float d = (two_layers && y >= kHeight / 2) ? 200.0f : 20.0f;
                sc.window_depth[i] = window_depth_of(d);
            }
            sc.normal_xy[i * 2 + 0] = 0.0f;
            sc.normal_xy[i * 2 + 1] = ny;
            // 颜色 = 格号的可逆编码（+1 / N+1 是为了让 (0,0) 也不是纯黑，
            // 否则"命中却取到黑"这条断言会被格号 0 误伤）。
            sc.color[i * 4 + 0] = static_cast<float>(x + 1) / (kWidth + 1);
            sc.color[i * 4 + 1] = static_cast<float>(y + 1) / (kHeight + 1);
            sc.color[i * 4 + 2] = 0.25f;
            sc.color[i * 4 + 3] = 1.0f;
        }
    }
    return sc;
}

unsigned int UploadFloatTexture(const std::vector<float>& data, int w, int h, GLint internal,
                                GLenum format) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// 把纹理挂到临时 FBO 上读回（R32F / RGBA16F 走 GL_FLOAT，R8UI 走 GL_UNSIGNED_BYTE）。
std::vector<float> ReadTexFloat(unsigned int tex, int w, int h, GLenum format) {
    unsigned int fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    const int comps = (format == GL_RGBA) ? 4 : 1;
    std::vector<float> out(static_cast<size_t>(w) * h * comps, 0.0f);
    glReadPixels(0, 0, w, h, format, GL_FLOAT, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return out;
}

std::vector<unsigned char> ReadTexUInt8(unsigned int tex, int w, int h) {
    unsigned int fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    std::vector<unsigned char> out(static_cast<size_t>(w) * h, 0);
    glReadPixels(0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return out;
}

// ───────────────────────── CPU 参考（与着色器同一套 ssr_math 函数）─────────────────────────

struct CpuResult {
    bool hit = false;
    float s = kNoHit;
    int cell_x = -1;
    int cell_y = -1;
    float confidence = 0.0f;     // A（仅 hit 时非 0）
    bool saw_backface = false;   // 命中过柱体但朝向不对（融合层拿它压暗 env）
};

// 逐个像素算 CPU 参考；depth_field 用窗口深度反算视距，与 GPU 完全同源。
// use_facing = true 时把命中面朝向判据也带上（attr_z ≥ 0.5 的格才判）。
CpuResult MarchPixelCpu(const Scene& sc, int px, int py, const DerivedParams& derived,
                        float max_distance_m, bool use_facing = false,
                        float max_ray_pixels = 0.0f, float fade_budget_px = 0.0f,
                        float fade_edge_px = 0.0f,
                        const std::vector<jpov::ssr::CellZRange>* range = nullptr) {
    const size_t i = static_cast<size_t>(py) * kWidth + px;
    CpuResult out;
    // 与着色器同一条规则：没有深度数据 ⇒ 不发射线、a=0（ssr_math.h::HasDepthData）。
    if (!HasDepthData(sc.window_depth[i])) {
        return out;
    }
    const float d_surf = jpov::ssr::DepthFromWindowDepth(sc.window_depth[i], kNear, kFar);
    const Vec3 n = ReconstructNormal(sc.normal_xy[i * 2 + 0], sc.normal_xy[i * 2 + 1]);
    Vec3 p = ViewPointAtUv((px + 0.5f) / kWidth, (py + 0.5f) / kHeight, d_surf, kTanHalf, kAspect);
    const Vec3 r = ReflectDir(p, n);
    p = OffsetRayOrigin(p, n, d_surf);
    const float len = ClipRayLength(p, r, kNear, kFar, max_distance_m);
    ScreenRay ray;
    if (!BuildScreenRay(p, r, len, kTanHalf, kAspect, kWidth, kHeight, &ray)) {
        return out;
    }
    // 回调返回 **w = 1/z**（0 = 空），与着色器读的降采样纹理同一种形式。
    auto depth_at = [&](int x, int y) -> float {
        const size_t k = static_cast<size_t>(y) * kWidth + x;
        return jpov::ssr::WFromWindowDepth(sc.window_depth[k], kNear, kFar);
    };
    int cx = -1;
    int cy = -1;
    auto facing = [&](int x, int y, const Vec3& d) -> bool {
        if (!use_facing) {
            return true;
        }
        const size_t k = static_cast<size_t>(y) * kWidth + x;
        if (sc.attr_z.empty() || sc.attr_z[k] < 0.5f) {
            return true;   // 没有法线数据：不判方向（与着色器 attr.z < 0.5 一致）
        }
        const Vec3 n = ReconstructNormal(sc.normal_xy[k * 2 + 0], sc.normal_xy[k * 2 + 1]);
        return jpov::ssr::HitFacesRay(d, n);
    };
    // 放宽后的 z 区间（面斜率补偿）：与着色器读同一张 RG32F 纹理。
    auto range_at = [&](int x, int y, float* w_lo, float* w_hi) -> bool {
        if (range == nullptr || range->empty()) {
            return false;
        }
        const jpov::ssr::CellZRange& r = (*range)[static_cast<size_t>(y) * kWidth + x];
        if (!(r.z_lo > 0.0f) || !(r.z_hi > 0.0f)) {
            return false;
        }
        *w_lo = 1.0f / r.z_lo;
        *w_hi = 1.0f / r.z_hi;
        return true;
    };
    const float s = MarchSlabs(ray, derived, 1.0e-4f, depth_at, kWidth, kHeight, &cx, &cy, facing,
                               &out.saw_backface, range_at);
    if (s < kNoHit) {
        out.hit = true;
        out.s = s;
        out.cell_x = cx;
        out.cell_y = cy;
        out.confidence = jpov::ssr::HitConfidence(
            ray, s, max_ray_pixels,
            jpov::ssr::FadeParams{/*budget=*/fade_budget_px, /*refl_edge=*/fade_edge_px,
                                  /*hit_edge=*/0.0f});
    }
    return out;
}

// ───────────────────────── 用例 ─────────────────────────

struct Fixture {
    SsrRenderer ssr;
    jpov::ShaderManager shader_mgr;
    Scene scene;
    unsigned int w_tex = 0;
    unsigned int normal_tex = 0;
    unsigned int color_tex = 0;
    unsigned int rough_tex = 0;
    unsigned int range_tex = 0;
    SsrInputs in;

    // 面斜率补偿的输入（RG32F：w_lo, w_hi）。不调用 ⇒ in.zrange_tex = 0 ⇒ 分支走不到。
    void UploadRange(const std::vector<float>& rg) {
        glGenTextures(1, &range_tex);
        glBindTexture(GL_TEXTURE_2D, range_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, kWidth, kHeight, 0, GL_RG, GL_FLOAT, rg.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        in.zrange_tex = range_tex;
    }

    void Setup(const Scene& sc, int tile, int max_ray_pixels) {
        GlEnv::Get().Ensure();
        scene = sc;
        // 上传 **w = 1/z**（2026-11-02 起 SSR 模块内部的深度形式；0 = 天空）。
        {
            std::vector<float> ws(scene.window_depth.size());
            for (size_t i = 0; i < ws.size(); ++i) {
                ws[i] = jpov::ssr::WFromWindowDepth(scene.window_depth[i], kNear, kFar);
            }
            w_tex = UploadFloatTexture(ws, kWidth, kHeight, GL_R32F, GL_RED);
        }
        // 法线用 RG32F（**不是** RG16F）：fp16 会把 ny=0.95 量化成 0.95019，
        // CPU 参考拿到的却是 0.95 —— 逐像素对拍时这点差异会在柱子边缘把命中格推走一格。
        // 真实管线用 fp16 也没问题（模块只做归一化与反射），只是**对拍**必须两边同值。
        // 场景带了 attr_z ⇒ 用 RGBA32F：.z = 1 表示该 texel 有法线数据（朝向判据用）。
        // 2026-10-31 起属性纹理语义：**rgb = 完整三维视空间法线**，a = 掩码编码
        // （1 反射体 / 0.5 有法线 / 0 无数据）。不再用 .xy + 重建 z。
        //   · nz 由 xy 现算（与 CPU 参考的 ReconstructNormal 一致）；
        //   · 老用例没有 attr_z ⇒ 语义是"这一格**不判朝向**"（当时靠 .z=0 表达），
        //     新编码用 a<0.25 表达；法线 xyz 仍然照写（射线构造要用，不能是 NaN）。
        {
            std::vector<float> rgba(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
            for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
                const float nx = scene.normal_xy[i * 2 + 0];
                const float ny = scene.normal_xy[i * 2 + 1];
                const bool judge_facing = !scene.attr_z.empty() && scene.attr_z[i] >= 0.5f;
                rgba[i * 4 + 0] = nx;
                rgba[i * 4 + 1] = ny;
                rgba[i * 4 + 2] = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                rgba[i * 4 + 3] = judge_facing ? 1.0f : 0.0f;
            }
            normal_tex = UploadFloatTexture(rgba, kWidth, kHeight, GL_RGBA32F, GL_RGBA);
        }
        color_tex = UploadFloatTexture(scene.color, kWidth, kHeight, GL_RGBA16F, GL_RGBA);
        // 逐像素 roughness（2026-11-02 起是必需输入）。本文件测的是**几何/取色**，
        // 与材质无关 ⇒ 全 0 + cutoff=1（永不跳过），等价于旧行为、不改变任何断言。
        {
            std::vector<float> ones(static_cast<size_t>(kWidth) * kHeight, 0.0f);
            rough_tex = UploadFloatTexture(ones, kWidth, kHeight, GL_R16F, GL_RED);
        }
        ssr.Init(&shader_mgr);
        in.color_tex = color_tex;
        in.w_tex = w_tex;
        in.normal_tex = normal_tex;
        in.rough_tex = rough_tex;
        in.roughness_cutoff = 1.0f;
        in.width = kWidth;
        in.height = kHeight;
        in.near_plane = kNear;
        in.far_plane = kFar;
        in.tan_half_fov_y = kTanHalf;
        in.aspect = kAspect;
        in.thickness_pixels = 10.0f;
        in.min_thickness_m = 1.0e-4f;
        in.max_distance_m = 900.0f;
        in.max_ray_pixels = max_ray_pixels;
        in.tile_size = tile;
        in.debug_reason = false;
    }

    ~Fixture() {
        ssr.Finalize();
        if (w_tex != 0) {
            glDeleteTextures(1, &w_tex);
        }
        if (normal_tex != 0) {
            glDeleteTextures(1, &normal_tex);
        }
        if (color_tex != 0) {
            glDeleteTextures(1, &color_tex);
        }
        if (rough_tex != 0) {
            glDeleteTextures(1, &rough_tex);
        }
        if (range_tex != 0) {
            glDeleteTextures(1, &range_tex);
        }
    }
};

// ① CPU 参考对拍
TEST(SsrGpu, MatchesCpuReference) {
    for (int tile : {1, 2, 4, 8}) {
        for (bool two_layers : {false, true}) {
            Fixture f;
            // 顶部 8 行是天空 ⇒ 对拍也覆盖"无深度数据"这条分支（真实场景里必然出现）。
            f.Setup(MakeScene(two_layers, /*ny=*/0.95f, /*sky_rows=*/8), tile, 160);
            f.in.debug_reason = true;   // 要 hit_s 才能对拍 s
            const SsrOutputs& out = f.ssr.Render(f.in);
            ASSERT_NE(out.radiance_tex, 0u);

            const DerivedParams derived =
                DeriveParams(tile, f.in.max_ray_pixels, f.in.thickness_pixels, kTanHalf, kHeight);
            const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
            // A 不再是 radiance.a（那里现在是 ray_ratio）：命中与否由类型图给出。
            const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);
            const std::vector<float> hit_s = ReadTexFloat(out.hit_s_tex, kWidth, kHeight, GL_RED);

            int sky_pixels = 0;
            int sky_with_alpha = 0;
            int compared = 0;
            int hit_mismatch = 0;
            int cell_mismatch = 0;
            int s_mismatch = 0;
            float worst_s = 0.0f;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const size_t i = static_cast<size_t>(y) * kWidth + x;
                    const CpuResult cpu = MarchPixelCpu(f.scene, x, y, derived, f.in.max_distance_m);
                    const bool gpu_hit = type[i] == static_cast<unsigned char>(SsrResult::kHit);
                    ++compared;
                    if (!HasDepthData(f.scene.window_depth[i])) {
                        ++sky_pixels;
                        if (gpu_hit) {
                            ++sky_with_alpha;
                        }
                    }
                    if (cpu.hit != gpu_hit) {
                        ++hit_mismatch;
                        if (hit_mismatch < 4) {
                            ADD_FAILURE() << "tile=" << tile << " two_layers=" << two_layers
                                          << " 像素(" << x << "," << y << ") CPU hit=" << cpu.hit
                                          << " GPU type=" << static_cast<int>(type[i]);
                        }
                        continue;
                    }
                    if (!gpu_hit) {
                        continue;
                    }
                    // 命中 ⇒ 颜色必须是命中格的那一格颜色（R/G 编码了格号，可反查）
                    const int cell_x =
                        static_cast<int>(std::lround(rad[i * 4 + 0] * (kWidth + 1))) - 1;
                    const int cell_y =
                        static_cast<int>(std::lround(rad[i * 4 + 1] * (kHeight + 1))) - 1;
                    if (cell_x != cpu.cell_x || cell_y != cpu.cell_y) {
                        ++cell_mismatch;
                        if (cell_mismatch < 4) {
                            ADD_FAILURE() << "tile=" << tile << " 像素(" << x << "," << y
                                          << ") 命中格: CPU=(" << cpu.cell_x << ","
                                          << cpu.cell_y << ") GPU=(" << cell_x << "," << cell_y
                                          << ")";
                        }
                    }
                    // s 对拍：两边都应落在同一个格内，中点位置最多差一点点（命中格若被
                    // 段边界切开，中点会不同，但**格**必须相同 —— 所以 s 只做宽松约束）。
                    const float ds = std::fabs(hit_s[i] - cpu.s);
                    worst_s = std::max(worst_s, ds);
                    if (ds > 2.0f) {
                        ++s_mismatch;
                    }
                }
            }
            EXPECT_GT(compared, 5000);
            EXPECT_GT(sky_pixels, 300) << "场景里没有天空像素 ⇒ HasDepthData 那条分支没被覆盖";
            EXPECT_EQ(sky_with_alpha, 0) << "天空像素（无深度数据）必须是 type=None（不是命中）";
            EXPECT_EQ(hit_mismatch, 0) << "tile=" << tile
                                       << "：CPU 与 GPU 的命中/未命中判定不一致"
                                          "（着色器与 ssr_math.h 走样了）";
            EXPECT_EQ(cell_mismatch, 0) << "tile=" << tile << "：命中格不一致";
            EXPECT_EQ(s_mismatch, 0) << "tile=" << tile << "：CPU 与 GPU 的命中位置 s 差 > 2 px"
                                     << "（worst=" << worst_s << "）";
        }
    }
}

// ①-b 面斜率补偿（鲁棒版）的 CPU 对拍
//
// 着色器那侧的数据来自 SsrDownsample 的第二趟（前置放宽 z 区间）。本用例在 CPU 上
// 用**同一个纯函数** WidenCellZ 算一遍、上传成 RG32F，再逐像素对拍命中/命中格/s。
TEST(SsrGpu, SlopeFixMatchesCpuReference) {
    for (bool two_layers : {false, true}) {
        Fixture f;
        f.Setup(MakeScene(two_layers, /*ny=*/0.95f, /*sky_rows=*/8), /*tile=*/2, 160);
        // 把旧厚度压到很小：否则旧判据本来就命中绝大多数格，扩区间分支永远跑不到
        // （那样这条用例是空转）。压小之后"旧判据漏、扩区间补"才会真的发生。
        f.in.thickness_pixels = 0.25f;

        const float fov_y = 2.0f * std::atan(kTanHalf);
        const float cell_angle = fov_y / static_cast<float>(kHeight);
        const float thickness_c = f.in.thickness_pixels * fov_y / static_cast<float>(kHeight);
        std::vector<jpov::ssr::CellZRange> range(
            static_cast<size_t>(kWidth) * kHeight);
        std::vector<float> rg(range.size() * 2, 0.0f);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                const float w = jpov::ssr::WFromWindowDepth(f.scene.window_depth[i], kNear, kFar);
                if (!(w > 0.0f)) {
                    continue;
                }
                const float z = 1.0f / w;
                const Vec3 n = ReconstructNormal(f.scene.normal_xy[i * 2 + 0],
                                                 f.scene.normal_xy[i * 2 + 1]);
                float zmin3 = z;
                float zmax3 = z;
                for (int jj = -1; jj <= 1; ++jj) {
                    for (int ii = -1; ii <= 1; ++ii) {
                        const int qx = std::min(std::max(x + ii, 0), kWidth - 1);
                        const int qy = std::min(std::max(y + jj, 0), kHeight - 1);
                        const float wq = jpov::ssr::WFromWindowDepth(
                            f.scene.window_depth[static_cast<size_t>(qy) * kWidth + qx], kNear, kFar);
                        if (wq > 0.0f) {
                            zmin3 = std::min(zmin3, 1.0f / wq);
                            zmax3 = std::max(zmax3, 1.0f / wq);
                        }
                    }
                }
                range[i] = jpov::ssr::WidenCellZ(z, jpov::ssr::CellNormalSlope(n.x, n.y, n.z),
                                                 cell_angle, thickness_c, zmin3, zmax3, kFar);
                rg[i * 2 + 0] = (range[i].z_lo > 0.0f) ? 1.0f / range[i].z_lo : 0.0f;
                rg[i * 2 + 1] = (range[i].z_hi > 0.0f) ? 1.0f / range[i].z_hi : 0.0f;
            }
        }
        f.UploadRange(rg);
        f.in.slope_fix = true;
        f.in.debug_reason = true;
        const SsrOutputs& out = f.ssr.Render(f.in);
        ASSERT_NE(out.radiance_tex, 0u);

        const DerivedParams derived =
            DeriveParams(f.in.tile_size, f.in.max_ray_pixels, f.in.thickness_pixels, kTanHalf,
                         kHeight);
        const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
        const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);
        const std::vector<float> hit_s = ReadTexFloat(out.hit_s_tex, kWidth, kHeight, GL_RED);

        int compared = 0;
        int hit_mismatch = 0;
        int cell_mismatch = 0;
        float worst_s = 0.0f;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                const CpuResult cpu = MarchPixelCpu(f.scene, x, y, derived, f.in.max_distance_m,
                                                    /*use_facing=*/false, 0.0f, 0.0f, 0.0f,
                                                    &range);
                const bool gpu_hit = type[i] == static_cast<unsigned char>(SsrResult::kHit);
                ++compared;
                if (cpu.hit != gpu_hit) {
                    ++hit_mismatch;
                    if (hit_mismatch < 4) {
                        ADD_FAILURE() << "像素(" << x << "," << y << ") CPU hit=" << cpu.hit
                                      << " GPU type=" << static_cast<int>(type[i]);
                    }
                    continue;
                }
                if (!gpu_hit) {
                    continue;
                }
                const int cell_x =
                    static_cast<int>(std::lround(rad[i * 4 + 0] * (kWidth + 1))) - 1;
                const int cell_y =
                    static_cast<int>(std::lround(rad[i * 4 + 1] * (kHeight + 1))) - 1;
                if (cell_x != cpu.cell_x || cell_y != cpu.cell_y) {
                    ++cell_mismatch;
                }
                worst_s = std::max(worst_s, std::fabs(hit_s[i] - cpu.s));
            }
        }
        EXPECT_GT(compared, 5000);
        EXPECT_EQ(hit_mismatch, 0) << "开面斜率补偿后 CPU 与 GPU 的命中判定不一致";
        EXPECT_EQ(cell_mismatch, 0) << "命中的格必须一致";
        EXPECT_LT(worst_s, 2.0f) << "命中位置 s 差 > 2 px（worst=" << worst_s << "）";

        // 注：本合成场景是"均匀深度墙"，射线必然穿过柱子 ⇒ 旧判据本来就命中，
        // 扩区间分支几乎跑不到（实测把区间收成零厚度，type 图也一个像素不变）。
        // 所以**这里不做活性断言**（做了也是假的）；该分支的有效性由真实机位验证：
        // 漏光相机 `--capture-slope` 下漏点 395 → 3（见 docs/jpov_ssr_v2_design.md §9.5）。
    }
}

// ② 手推的定性结论（不依赖 CPU 实现）：均匀深度墙 + 地板法线
//
// 几何：镜面在 d=20 m，柱子 [20, 20+T]（T = c·20 ≈ 3.3 m @ 这个 64 px 高的假相机）。
// 反射射线从 19.994 m（起点沿法线抬高了一个亚像素量）出发、**d 单调增加** ⇒ 它一定会在
// 出发后极短的距离内穿过 [20, 20+T] ⇒ **几乎每个像素都命中，且 s 只有亚像素~几像素**。
// 唯一合法的例外是"射线在一个像素之内就飞出屏幕"的贴边像素（出屏，a=0）。
//
// 这条测试的杀伤力在方向约定：若把 R 写成 reflect(−I, N)（朝相机），射线会被近平面裁到
// 极短、屏幕足迹朝外飞 ⇒ 命中率立刻塌到接近 0，下面的 hit_rate 断言直接红。
TEST(SsrGpu, HitPatternFollowsReflectedDirection) {
    Fixture f;
    f.Setup(MakeScene(/*two_layers=*/false), /*tile=*/4, /*max_ray_pixels=*/160);
    f.in.debug_reason = true;
    const SsrOutputs& out = f.ssr.Render(f.in);

    const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> reason = ReadTexUInt8(out.type_tex, kWidth, kHeight);
    const std::vector<float> hit_s = ReadTexFloat(out.hit_s_tex, kWidth, kHeight, GL_RED);

    constexpr int kMargin = 3;
    int hits = 0;
    int interior = 0;
    int bad_reason = 0;
    int far_hits = 0;
    int far_cell = 0;
    int up_dir = 0;
    int down_dir = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const bool hit = reason[i] == static_cast<unsigned char>(SsrResult::kHit);
            const bool is_interior = x >= kMargin && x < kWidth - kMargin && y >= kMargin &&
                                     y < kHeight - kMargin;
            if (is_interior) {
                ++interior;
            }
            if (hit) {
                ++hits;
                EXPECT_EQ(reason[i], static_cast<unsigned char>(SsrResult::kHit));
                // 命中格紧贴起点：s 只有亚像素~几像素（柱子 T≈3.3 m，走 0.006 m 就进柱体）。
                if (hit_s[i] > 6.0f) {
                    ++far_hits;
                }
                const int cell_x =
                    static_cast<int>(std::lround(rad[i * 4 + 0] * (kWidth + 1))) - 1;
                const int cell_y =
                    static_cast<int>(std::lround(rad[i * 4 + 1] * (kHeight + 1))) - 1;
                // 命中必须发生在**紧挨起点**的格里：柱子只有 0.006 m 远（起点抬高量），
                // 射线在亚像素距离内就穿进 [20,20+T] ⇒ 差超过 2 格就说明 1/d 或 T 错了。
                if (std::abs(cell_x - x) > 2 || std::abs(cell_y - y) > 2) {
                    ++far_cell;
                }
                // 屏幕方向的两个符号都必须出现：地板法线的反射射线朝**消失点**收敛，
                // 消失点在屏幕中间偏上 ⇒ 上面的像素命中在下、下面的命中在上。
                if (cell_y > y) {
                    ++up_dir;
                } else if (cell_y < y) {
                    ++down_dir;
                }
            } else if (reason[i] == static_cast<unsigned char>(SsrResult::kHit)) {
                ++bad_reason;
            }
        }
    }
    const int total = kWidth * kHeight;
    EXPECT_GT(hits, total * 6 / 10) << "命中率过低（" << hits << "/" << total
                                    << "）⇒ 反射方向约定或 1/d 反算有误";
    // 注：肯定有一部分像素合法地"不命中"——反射射线恰好**平行于深度带**（R.z≈0）时，
    // 它会一直贴在柱子前面 0.006 m 处飞，永远进不去 [20, 20+T]。这是均匀深度墙这种
    // 合成场景的性质，不是 bug（真实场景里墙和地板不在同一深度）。
    EXPECT_EQ(far_hits, 0) << "有 " << far_hits << " 个命中的 s > 6 px —— 均匀深度墙下"
                              "命中必须紧贴起点格（1/d 或厚度算式有误）";
    EXPECT_EQ(far_cell, 0) << "有 " << far_cell << " 个像素的命中格离起点 > 2 格"
                              "（均匀深度墙下命中必须紧贴起点格）";
    EXPECT_GT(up_dir, total / 20) << "几乎没有'命中在起点格上方'的像素";
    EXPECT_GT(down_dir, total / 20) << "几乎没有'命中在起点格下方'的像素"
                                       "（射线应朝消失点收敛，两种情况都该有）";
    EXPECT_EQ(bad_reason, 0) << "未命中的像素却写了 kHit";
    EXPECT_GT(interior, total / 2);
}

// ③ tile 只是性能旋钮：1/2/4/8 输出逐字节相同
TEST(SsrGpu, TileIsOnlyPerformance) {
    const Scene sc = MakeScene(/*two_layers=*/true);
    std::vector<float> ref_rad;
    std::vector<float> ref_s;
    for (int tile : {1, 2, 4, 8}) {
        Fixture f;
        f.Setup(sc, tile, /*max_ray_pixels=*/160);
        f.in.debug_reason = true;
        const SsrOutputs& out = f.ssr.Render(f.in);
        const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
        const std::vector<float> hit_s = ReadTexFloat(out.hit_s_tex, kWidth, kHeight, GL_RED);
        if (tile == 1) {
            ref_rad = rad;
            ref_s = hit_s;
            continue;
        }
        ASSERT_EQ(rad.size(), ref_rad.size()) << "tile=" << tile;
        int diff_px = 0;
        for (size_t i = 0; i < rad.size(); i += 4) {
            if (std::memcmp(&rad[i], &ref_rad[i], sizeof(float) * 4) != 0) {
                ++diff_px;
            }
        }
        EXPECT_EQ(diff_px, 0) << "tile=" << tile
                              << " 与 tile=1 的 radiance 有 " << diff_px
                              << " 个像素不同（tile 不该改变画面）";
        // hit_s **不**要求逐字节相同：s 是"命中格内的中点"，段边界一变中点就会动；
        // 但它必须落在**同一格**里（颜色因此不变，上面那条才是契约）。这里只卡上界。
        int far_s = 0;
        for (size_t i = 0; i < hit_s.size(); ++i) {
            const bool a_hit = hit_s[i] < kNoHit;
            const bool b_hit = ref_s[i] < kNoHit;
            if (a_hit != b_hit) {
                ++far_s;
            } else if (a_hit && std::fabs(hit_s[i] - ref_s[i]) > 2.0f) {
                ++far_s;
            }
        }
        EXPECT_EQ(far_s, 0) << "tile=" << tile << " 的 hit_s 与 tile=1 相差超过 2 px（"
                            << far_s << " 像素）—— 中点可以挪，但不能换格";
    }
}

// ④ A 三态：全天空 ⇒ kTooShort(a=0)；命中 ⇒ kHit(a=1)
TEST(SsrGpu, CoverageStates) {
    // 全天空：深度全是 1.0（远平面清空值）⇒ 不发射线、a 全 0、reason 全是 kTooShort。
    {
        Scene sc = MakeScene(/*two_layers=*/false);
        for (float& d : sc.window_depth) {
            d = 1.0f;
        }
        Fixture f;
        f.Setup(sc, /*tile=*/2, /*max_ray_pixels=*/160);
        f.in.debug_reason = true;
        const SsrOutputs& out = f.ssr.Render(f.in);
        const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
        const std::vector<unsigned char> reason = ReadTexUInt8(out.type_tex, kWidth, kHeight);
        int nonzero_a = 0;
        int wrong_reason = 0;
        for (int i = 0; i < kWidth * kHeight; ++i) {
            if (rad[static_cast<size_t>(i) * 4 + 3] != 0.0f) {
                ++nonzero_a;
            }
            if (reason[i] != static_cast<unsigned char>(SsrResult::kNone)) {
                ++wrong_reason;
            }
        }
        EXPECT_EQ(nonzero_a, 0) << "无深度数据时 A 必须全 0";
        EXPECT_EQ(wrong_reason, 0) << "无深度数据时 type 必须是 kNone";
    }
    // 全命中：均匀深度墙的下半屏 ⇒ a=1 且颜色是某一格的颜色（不是全黑）。
    {
        Fixture f;
        f.Setup(MakeScene(/*two_layers=*/false), /*tile=*/2, /*max_ray_pixels=*/160);
        const SsrOutputs& out = f.ssr.Render(f.in);
        const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
        const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);
        int lit = 0;
        int black_but_hit = 0;
        for (int y = 0; y < kHeight / 4; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                if (type[i] == static_cast<unsigned char>(SsrResult::kHit)) {
                    ++lit;
                    if (rad[i * 4 + 0] == 0.0f && rad[i * 4 + 1] == 0.0f) {
                        ++black_but_hit;
                    }
                }
            }
        }
        EXPECT_GT(lit, kWidth * (kHeight / 4) * 8 / 10);
        EXPECT_EQ(black_but_hit, 0) << "命中却取到全黑 ⇒ 命中格 uv 或颜色取样有误";
    }
}

// ⑤ 退化输入：near≈0 / 法线 xy 越界 / max_ray_pixels 极小 —— 不许出 NaN，不许全黑全白
TEST(SsrGpu, DegenerateInputsAreFinite) {
    Fixture f;
    Scene sc = MakeScene(/*two_layers=*/false);
    // 法线 xy 长度 1.4（越界）+ 少量像素清 0（未写法线的反射体）
    for (size_t i = 0; i < sc.normal_xy.size(); i += 2) {
        sc.normal_xy[i] = 1.4f;
        sc.normal_xy[i + 1] = 0.2f;
    }
    f.Setup(sc, /*tile=*/4, /*max_ray_pixels=*/160);
    f.in.near_plane = 1.0e-9f;   // 退化相机：深度反算的分母趋 0
    f.in.debug_reason = true;
    const SsrOutputs& out = f.ssr.Render(f.in);
    const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
    int bad = 0;
    for (float v : rad) {
        if (!std::isfinite(v)) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0) << "输出里出现 NaN/inf（会顺着后处理污染整帧）";

    // max_ray_pixels 极小（4 px）⇒ 只能命中最靠前的一格，远处像素应判未命中。
    Fixture g;
    g.Setup(MakeScene(/*two_layers=*/true), /*tile=*/1, /*max_ray_pixels=*/4);
    const SsrOutputs& out2 = g.ssr.Render(g.in);
    const std::vector<float> rad2 = ReadTexFloat(out2.radiance_tex, kWidth, kHeight, GL_RGBA);
    int bad2 = 0;
    for (float v : rad2) {
        if (!std::isfinite(v)) {
            ++bad2;
        }
    }
    EXPECT_EQ(bad2, 0);
}

// ⑦ 预算填多大都不会挂、也不会改画面（2026-10-29 Danis 提出的"别加没用的保险"）
//
// 段尾是 `end = min(start + L, ray.exit)`，而 ray.exit ≤ 屏幕对角线（裁剪后）⇒ DDA 走到段尾
// 就 return ⇒ **循环次数自限**，与 uStepsPerLane 这个上限无关。所以不需要"把预算钳到对角线"
// 这种保险，也不需要为它打 warning。这条测试就是那句论断的凭据：预算 1000 与 1000000
// （都远大于 96×64 的对角线 ≈ 115）必须产出**逐字节相同**的画面，且跑得完。
TEST(SsrGpu, HugeBudgetIsHarmless) {
    const Scene sc = MakeScene(/*two_layers=*/true);
    Fixture modest;
    modest.Setup(sc, /*tile=*/1, /*max_ray_pixels=*/1000);
    const SsrOutputs& a = modest.ssr.Render(modest.in);
    const std::vector<float> rad_a = ReadTexFloat(a.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> type_a = ReadTexUInt8(a.type_tex, kWidth, kHeight);

    Fixture huge;
    huge.Setup(sc, /*tile=*/1, /*max_ray_pixels=*/1000000);
    const SsrOutputs& b = huge.ssr.Render(huge.in);
    const std::vector<float> rad_b = ReadTexFloat(b.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> type_b = ReadTexUInt8(b.type_tex, kWidth, kHeight);

    int bad = 0;
    int diff_rgb = 0;
    int diff_type = 0;
    for (size_t i = 0; i < rad_b.size(); ++i) {
        if (!std::isfinite(rad_b[i])) {
            ++bad;
        }
        // 只比 rgb：a 是 **ray_ratio = s / max_ray_pixels**，预算变了它本来就该变
        // （分母就是预算本身），这不属于"改了画面"。
        if ((i % 4) != 3 && rad_a[i] != rad_b[i]) {
            ++diff_rgb;
        }
    }
    for (size_t i = 0; i < type_b.size(); ++i) {
        if (type_a[i] != type_b[i]) {
            ++diff_type;
        }
    }
    EXPECT_EQ(bad, 0) << "超大预算产出了 NaN/inf";
    EXPECT_EQ(diff_rgb, 0) << "预算超过屏幕对角线竟然改了命中颜色（" << diff_rgb << " 个分量不同）";
    EXPECT_EQ(diff_type, 0) << "预算变化改了命中/逃逸分类（" << diff_type << " 个像素不同）";
}

// ⑥ 决定论：同输入跑两次逐字节相同
TEST(SsrGpu, Deterministic) {
    Fixture f;
    f.Setup(MakeScene(/*two_layers=*/true), /*tile=*/4, /*max_ray_pixels=*/160);
    f.in.debug_reason = true;
    const SsrOutputs& a = f.ssr.Render(f.in);
    const std::vector<float> rad_a = ReadTexFloat(a.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<float> s_a = ReadTexFloat(a.hit_s_tex, kWidth, kHeight, GL_RED);
    const SsrOutputs& b = f.ssr.Render(f.in);
    const std::vector<float> rad_b = ReadTexFloat(b.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<float> s_b = ReadTexFloat(b.hit_s_tex, kWidth, kHeight, GL_RED);
    EXPECT_EQ(std::memcmp(rad_a.data(), rad_b.data(), rad_a.size() * sizeof(float)), 0)
        << "同一帧跑两次结果不同（决定论被破坏）";
    EXPECT_EQ(std::memcmp(s_a.data(), s_b.data(), s_a.size() * sizeof(float)), 0);
}

// ⑦ 命中面朝向：背面命中（射线从物体背后穿过去）必须不算命中
//
// 2026-10-30 Danis 实拍发现：体素柱/桌子这种厚物体，镜子里的射线从它**背面**穿过去，
// 旧判据却把**正面**（带高光）的像素当结果 ⇒ 倒影里桌子底下也在发光。
//
// 场景（屏幕空间直给，y=0 在底部）：
//   0..23   地板：d=12，法线 (0,0.95)（掠射反射体，射线往上且远离相机）
//   24..39  空（无深度数据）——射线穿过去
//   40..55  “桌子顶面”：d=20，法线 (0,1)（朝上）
//           ⇒ 地板射线的视深在这一带恰好是 20 出头（比存下的桌面**更远**，差 < T）
//           ⇒ 旧判据命中这一格，但 R·N ≈ +0.57 > 0 = 从底面穿出 ⇒ 必须拒
//   56..63  “墙”：d=20，法线 (0,0)（朝相机）⇒ R·N < 0 ⇒ 正面命中 ⇒ 必须中
//
// 四条断言：① GPU 与 CPU（带朝向判据）逐像素一致（命中/未命中 + 命中格）；
//   ② 负向：CPU 不判朝向时确实在桌子带命中（这个场景真能造出 bug）；
//   ③ 正向：带判据后命中桌子带的像素数归 0；
//   ④ 防过严：带判据后仍有像素命中到墙那一段（不是把一切都拒了）。
constexpr int kFacingFloorRows = 24;    // 地板占 y ∈ [0,24)
constexpr int kFacingTableBegin = 40;   // 桌子带开始
constexpr int kFacingTableEnd = 56;     // 桌子带结束，墙从这行开始
constexpr float kFacingFloorDepth = 12.0f;
constexpr float kFacingTableDepth = 20.0f;

Scene MakeFacingScene() {
    Scene sc;
    sc.window_depth.assign(static_cast<size_t>(kWidth) * kHeight, 1.0f);
    sc.normal_xy.assign(static_cast<size_t>(kWidth) * kHeight * 2, 0.0f);
    sc.color.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    sc.attr_z.assign(static_cast<size_t>(kWidth) * kHeight, 0.0f);
    sc.kind.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    auto window_depth_of = [](float d) {
        const float ndc = ((kFar + kNear) - 2.0f * kNear * kFar / d) / (kFar - kNear);
        return 0.5f * ndc + 0.5f;
    };
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            float d = 0.0f;
            float ny = 0.0f;
            int kind = 0;
            if (y < kFacingFloorRows) {
                d = kFacingFloorDepth;
                ny = 0.95f;                       // 地板：掠射反射体，法线朝上
                kind = 1;
            } else if (y >= kFacingTableBegin && y < kFacingTableEnd) {
                d = kFacingTableDepth;
                ny = 1.0f;                        // 桌子顶面：法线朝上
                kind = 2;
            } else if (y >= kFacingTableEnd) {
                d = kFacingTableDepth;
                kind = 3;                         // 墙：法线朝相机（xy=0 ⇒ N=(0,0,1)）
            }
            sc.kind[i] = kind;
            sc.window_depth[i] = (kind != 0) ? window_depth_of(d) : 1.0f;   // 1.0 = 无深度
            sc.normal_xy[i * 2 + 0] = 0.0f;
            sc.normal_xy[i * 2 + 1] = ny;
            sc.attr_z[i] = (kind != 0) ? 1.0f : 0.0f;
            sc.color[i * 4 + 0] = static_cast<float>(x + 1) / (kWidth + 1);
            sc.color[i * 4 + 1] = static_cast<float>(y + 1) / (kHeight + 1);
            sc.color[i * 4 + 2] = 0.25f;
            sc.color[i * 4 + 3] = 1.0f;
        }
    }
    return sc;
}

TEST(SsrGpu, BackfaceHitsRejected) {
    Fixture f;
    f.Setup(MakeFacingScene(), /*tile=*/4, /*max_ray_pixels=*/160);
    f.in.debug_reason = true;
    const SsrOutputs& out = f.ssr.Render(f.in);
    ASSERT_NE(out.radiance_tex, 0u);

    const DerivedParams derived =
        DeriveParams(f.in.tile_size, f.in.max_ray_pixels, f.in.thickness_pixels, kTanHalf, kHeight);
    const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);

    int hit_mismatch = 0;
    int cell_mismatch = 0;
    int old_hits_in_table = 0;      // 负向：不判朝向时从地板撞在桌子带的像素
    int facing_hits_in_table = 0;   // 正向：判朝向后仍撞在桌子带的像素（应为 0）
    int facing_hits_in_wall = 0;    // 防过严：判朝向后仍命中墙的像素
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const CpuResult old_cpu =
                MarchPixelCpu(f.scene, x, y, derived, f.in.max_distance_m, /*use_facing=*/false);
            const CpuResult cpu =
                MarchPixelCpu(f.scene, x, y, derived, f.in.max_distance_m, /*use_facing=*/true);
            const bool gpu_hit = type[i] == static_cast<unsigned char>(SsrResult::kHit);

            // 只统计"地板像素射出去的射线"：桌子/墙自己的像素也会发射线（那属于它们自己的
            // 镜面，跟这条 bug 无关）。
            if (f.scene.kind[i] == 1) {
                if (old_cpu.hit && f.scene.kind[static_cast<size_t>(old_cpu.cell_y) * kWidth +
                                                 old_cpu.cell_x] == 2) {
                    ++old_hits_in_table;
                }
                if (cpu.hit) {
                    const int hit_kind =
                        f.scene.kind[static_cast<size_t>(cpu.cell_y) * kWidth + cpu.cell_x];
                    if (hit_kind == 2) {
                        ++facing_hits_in_table;
                    } else if (hit_kind == 3) {
                        ++facing_hits_in_wall;
                    }
                }
            }

            if (cpu.hit != gpu_hit) {
                ++hit_mismatch;
                if (hit_mismatch < 4) {
                    ADD_FAILURE() << "像素(" << x << "," << y << ") CPU hit=" << cpu.hit
                                  << " GPU type=" << static_cast<int>(type[i]);
                }
                continue;
            }
            if (!gpu_hit) {
                continue;
            }
            // 命中 ⇒ 颜色必须是命中格的那一格颜色（R/G 编码了格号，可反查）。
            const int cell_x = static_cast<int>(std::lround(rad[i * 4 + 0] * (kWidth + 1))) - 1;
            const int cell_y = static_cast<int>(std::lround(rad[i * 4 + 1] * (kHeight + 1))) - 1;
            if (cell_x != cpu.cell_x || cell_y != cpu.cell_y) {
                ++cell_mismatch;
                if (cell_mismatch < 4) {
                    ADD_FAILURE() << "像素(" << x << "," << y << ") CPU 命中格 (" << cpu.cell_x
                                  << "," << cpu.cell_y << ") GPU 取到 (" << cell_x << ","
                                  << cell_y << ")";
                }
            }
        }
    }
    EXPECT_EQ(hit_mismatch, 0) << "CPU（带朝向判据）与 GPU 命中不一致";
    EXPECT_EQ(cell_mismatch, 0) << "CPU（带朝向判据）与 GPU 命中格不一致";
    EXPECT_GT(old_hits_in_table, 20)
        << "不判朝向时没在桌子带命中 ⇒ 这个场景造不出那个 bug，测试没意义";
    EXPECT_EQ(facing_hits_in_table, 0) << "背面命中（射线从桌面下方穿出）竟然还算命中";
    EXPECT_GT(facing_hits_in_wall, 0) << "朝向判据把墙的正面命中一起拒掉了（判太严）";
}

// ⑥ 置信度 A（2026-11-06）：GPU 的 alpha 必须等于 CPU 的 HitConfidence。
// 这条同时锁四件事：
//   · resolve 算 A 用的是**与 CPU 同一套** s / span / sigma_exit（不是另外一套“射线进度”）；
//   · 两个带宽的单位是 SSR 像素（本用例 scale=1，所以它们就是屏幕像素）；
//   · A 真的落在 [0,1] 且没有全屏被衰减；
//   · 负向对照在 ⑦：带宽=0 时 A 必须恰为 1。
TEST(SsrGpu, ConfidenceMatchesCpu) {
    Fixture f;
    f.Setup(MakeScene(/*two_layers=*/true, /*ny=*/0.95f, /*sky_rows=*/8), /*tile=*/2,
            /*max_ray_pixels=*/160);
    const float kFadeBudget = 40.0f;
    const float kFadeEdge = 24.0f;
    f.in.fade_budget_pixels = kFadeBudget;
    f.in.fade_edge_pixels = kFadeEdge;
    const SsrOutputs& out = f.ssr.Render(f.in);

    const DerivedParams derived =
        DeriveParams(f.in.tile_size, f.in.max_ray_pixels, f.in.thickness_pixels, kTanHalf, kHeight);
    const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);

    int mismatch = 0;
    int faded = 0;
    int full = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            if (type[i] != static_cast<unsigned char>(SsrResult::kHit)) {
                continue;
            }
            const CpuResult cpu = MarchPixelCpu(
                f.scene, x, y, derived, f.in.max_distance_m, /*use_facing=*/false,
                static_cast<float>(f.in.max_ray_pixels), kFadeBudget, kFadeEdge);
            ASSERT_TRUE(cpu.hit) << "GPU 说是 kHit，CPU 参考却未命中（像素 " << x << "," << y << "）";
            const float a_gpu = rad[i * 4 + 3];
            // 容差 1e-2：A 存在 RGBA16F（~5e-4）+ GPU/CPU 的 s 允许差 0.05 px
            // （带宽 40 px、曲线最陡处斜率 1.5 ⇒ 最大 ≈ 2e-3）。再收紧就是测浮点噪声了。
            if (std::fabs(a_gpu - cpu.confidence) > 1.0e-2f) {
                ++mismatch;
                if (mismatch < 4) {
                    ADD_FAILURE() << "像素(" << x << "," << y << ") A_gpu=" << a_gpu
                                  << " A_cpu=" << cpu.confidence << " s=" << cpu.s;
                }
            }
            if (a_gpu < 0.999f) {
                ++faded;
            } else {
                ++full;
            }
        }
    }
    EXPECT_EQ(mismatch, 0) << "GPU 的 A 与 CPU 的 HitConfidence 不一致";
    EXPECT_GT(faded, 50) << "没有任何像素进衰减带 ⇒ 带宽/场景选得不对，这用例测不到东西";
    EXPECT_GT(full, 50) << "所有像素都被衰减了 ⇒ 带宽过大";
}

// ⑦ 带宽 = 0 ⇒ A 恒为 1（定义："0 = 关闭"，**不是**"衰减 0 像素"）。
// 这条是 ⑥ 的负向对照：若代码把"接近预算"直接写成 s/max（旧的 ray_ratio 写法），
// 这里就会出现大量 <1 的像素。
TEST(SsrGpu, ConfidenceZeroWidthIsIdentity) {
    Fixture f;
    f.Setup(MakeScene(/*two_layers=*/true, /*ny=*/0.95f, /*sky_rows=*/8), /*tile=*/2,
            /*max_ray_pixels=*/160);
    f.in.fade_budget_pixels = 0.0f;
    f.in.fade_edge_pixels = 0.0f;
    const SsrOutputs& out = f.ssr.Render(f.in);
    const std::vector<float> rad = ReadTexFloat(out.radiance_tex, kWidth, kHeight, GL_RGBA);
    const std::vector<unsigned char> type = ReadTexUInt8(out.type_tex, kWidth, kHeight);
    int hits = 0;
    int not_one = 0;
    for (int i = 0; i < kWidth * kHeight; ++i) {
        if (type[i] != static_cast<unsigned char>(SsrResult::kHit)) {
            continue;
        }
        ++hits;
        if (rad[static_cast<size_t>(i) * 4 + 3] != 1.0f) {
            ++not_one;
        }
    }
    EXPECT_GT(hits, 100) << "这个场景应该大量命中（否则断言没意义）";
    EXPECT_EQ(not_one, 0) << "带宽为 0 时 A 必须恰为 1（0 = 关闭衰减）";
}

// ⑧ 背面标记：GPU 的 flags.bit0 必须等于 CPU 的 saw_backface。
// 融合层靠这一位决定要不要把 env fallback 压暗（SsrConfig::backface_env_scale），
// 所以它“有没有传到纹理上”必须被钉住。
TEST(SsrGpu, BackfaceFlagMatchesCpu) {
    Fixture f;
    f.Setup(MakeFacingScene(), /*tile=*/4, /*max_ray_pixels=*/160);
    const SsrOutputs& out = f.ssr.Render(f.in);
    ASSERT_NE(out.flags_tex, 0u) << "resolve 必须总是输出背面标记";

    const DerivedParams derived =
        DeriveParams(f.in.tile_size, f.in.max_ray_pixels, f.in.thickness_pixels, kTanHalf, kHeight);
    const std::vector<unsigned char> flags = ReadTexUInt8(out.flags_tex, kWidth, kHeight);
    int mismatch = 0;
    int backface_px = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const CpuResult cpu =
                MarchPixelCpu(f.scene, x, y, derived, f.in.max_distance_m, /*use_facing=*/true);
            const bool gpu_flag = (flags[i] & 1u) != 0u;
            if (gpu_flag != cpu.saw_backface) {
                ++mismatch;
                if (mismatch < 4) {
                    ADD_FAILURE() << "像素(" << x << "," << y << ") flags=" << int(flags[i])
                                  << " cpu_saw_backface=" << cpu.saw_backface;
                }
            }
            if (gpu_flag) {
                ++backface_px;
            }
        }
    }
    EXPECT_EQ(mismatch, 0) << "GPU 的背面标记与 CPU 不一致";
    EXPECT_GT(backface_px, 20) << "这个场景本来就会穿过桌面的背面（见图头），标记却是 0 个像素";
}

}  // namespace

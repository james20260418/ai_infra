// JPOV 远景雾（DistanceFog）GPU 测试 —— 锁住 inject 链路本身。
//
// 为什么端到端做：远景雾没有独立的 pass，它只是 fog 管线里的一个**全局介质项**
// （inject 逐切片累加 (τ, S)），所以唯一能直接验证它的地方是 FogRenderer::Build 产出的
// froxel 场（scatter 纹理）。gold 图能看观感但读不出 τ 的绝对值，这里读得出来。
//
// 用例（盒内均匀 ⇒ 期望值可手算，与切片分布无关）：
//   1. 均匀盒：σ 常数、盒子罩住整个视锥 ⇒ 第 k 切片的累积 τ = σ·(z_{k+1} − z_near)，
//      S = color·(1 − e^{−τ})（内散射色 = 用户给定色，不吃光照）。
//   2. 关掉远景雾 + 无雾团 ⇒ 场无效（零开销，不是"跑一趟全零"）。
//   3. 指数软边：切片的光程跨过盒面时 0 < Δτ < σ·Δz（边缘被衰减），盒外 Δτ = 0
//      —— 防"硬边"（kUniform 会被 CHECK 拒，这里测的是剖面真的在衰减）。
//
// 负向验证（开发时实跑）：
//   · 把 inject 的 Δτ 从 σ·len 改成 σ（忘了乘段长）⇒ 用例 1 红；
//   · 把 S 从 color·(1−e^{−Δτ}) 改成 color·Δτ ⇒ 用例 1 红（大 τ 处发散）；
//   · 把 ProfileAtten 换成恒 1（硬边）⇒ 用例 3 红；
//   · 把 has_dist_fog 的早退改回 `fogs.empty()` ⇒ 用例 1 场无效、红。

#define GL_GLEXT_PROTOTYPES

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/fog/fog_renderer.h"
#include "tools/jpov/src/primitives3d/primitives3d_renderer.h"

namespace {

constexpr int kViewW = 64;
constexpr int kViewH = 64;
constexpr int kNz = 256;
constexpr int kTilePx = 16;
// z 分布：near 0.1 / far 200 ⇒ 生长系数 (2000)^{1/256} ≈ 1.0299（100 m 处切片 ~3 m）。
constexpr float kZNear = 0.1f;
constexpr float kZFar = 200.0f;

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
        cfg.title = "Distance Fog GPU Test";
        cfg.headless = true;
        app = std::make_unique<NullApp>(cfg);
        app->Init();
    }
};

// 相机：在 +z 一侧看向原点（与场景惯例一致）。fbo 尺寸 = 测试视口。
jpov::Camera MakeCamera() {
    jpov::Camera cam;
    cam.position = {0.0f, 0.0f, 10.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fov = 60.0f;
    cam.near = 0.1f;
    cam.far = 1000.0f;
    return cam;
}

jpov::FogParams MakeParams() {
    jpov::FogParams params;
    params.z_near = kZNear;
    params.z_far = kZFar;
    params.nz = kNz;
    params.tile_px = kTilePx;
    return params;
}

// 一块罩住整个视锥的远景雾（相机在内部，任意方向都走满整段 [z_near, z_far]）。
jpov::DistanceFog MakeEnclosingFog(float sigma, jpov::Color color) {
    jpov::DistanceFog fog;
    fog.enabled = true;
    fog.center = {0.0f, 0.0f, 0.0f};
    fog.size = {8000.0f, 8000.0f, 8000.0f};   // 半尺寸 4000 ≫ z_far
    fog.radius = 0.0f;
    fog.sigma = sigma;
    fog.color = color;
    fog.attenuation = jpov::FogAttenuation::kLinear;
    return fog;
}

struct Fixture {
    jpov::FogRenderer fog;
    jpov::ShaderManager shader_mgr;
    unsigned int read_fbo = 0;

    void Setup() {
        GlEnv::Get().Ensure();
        fog.Init(&shader_mgr);
        glGenFramebuffers(1, &read_fbo);
    }

    ~Fixture() {
        fog.Finalize();
        if (read_fbo != 0) {
            glDeleteFramebuffers(1, &read_fbo);
        }
    }

    // 读回 scatter 纹理的一个 texel（RGBA32F）。
    std::array<float, 4> ReadScatterTexel(unsigned int tex, int x, int y) {
        glBindFramebuffer(GL_FRAMEBUFFER, read_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        std::array<float, 4> px = {0.0f, 0.0f, 0.0f, 0.0f};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return px;
    }

    // 第 k 切片（= 累积到 z_{k+1}）在 (tile_col, tile_row) 柱里的 texel 坐标。
    std::pair<int, int> SliceTexel(const jpov::FogField& field, int tile_col, int tile_row,
                                   int k) const {
        return {tile_col * field.block_side + (k % field.block_side),
                tile_row * field.block_side + (k / field.block_side)};
    }
};

// ── 用例 1：均匀盒的 τ 与 S 都按闭式累积 ──
TEST(DistanceFogGpuTest, UniformFogMatchesClosedForm) {
    Fixture fx;
    fx.Setup();

    const float sigma = 0.02f;
    const jpov::Color color = {0.4f, 0.5f, 0.7f, 1.0f};
    const jpov::DistanceFog dist = MakeEnclosingFog(sigma, color);
    const jpov::Camera cam = MakeCamera();
    const jpov::FogParams params = MakeParams();
    float mvp[16];
    jpov::Primitives3DRenderer::BuildMVP(cam, kViewW, kViewH, mvp);

    jpov::FogField field;
    const jpov::FogLighting light;                      // 远景雾不吃光照 ⇒ 留默认
    const std::vector<jpov::PointFog> no_fogs;
    const std::vector<jpov::PointLight> no_lights;
    fx.fog.Build(no_fogs, no_lights, /*light_data_tex=*/0, cam, mvp, kViewW, kViewH,
                 params, light, /*distance_fog=*/&dist, std::nullopt, &field);

    ASSERT_TRUE(field.valid()) << "只有远景雾（无雾团）时也必须产出场";
    ASSERT_EQ(field.nz, kNz);
    ASSERT_GT(field.grid_cols, 0);
    ASSERT_GT(field.grid_rows, 0);

    // 切片边界 z_k = z_near·(z_far/z_near)^(k/nz)（与 CPU 预表同一公式）。
    const float growth = std::pow(kZFar / kZNear, 1.0f / static_cast<float>(kNz));
    const int mid_col = field.grid_cols / 2;
    const int mid_row = field.grid_rows / 2;
    for (int k : {0, 1, kNz / 2, kNz - 1}) {
        float zk1 = kZNear;
        for (int i = 0; i <= k; ++i) {
            zk1 *= growth;
        }
        const float expect_tau = sigma * (zk1 - kZNear);
        const float cover = 1.0f - std::exp(-expect_tau);
        const std::pair<int, int> t = fx.SliceTexel(field, mid_col, mid_row, k);
        const std::array<float, 4> px = fx.ReadScatterTexel(field.scatter_tex, t.first,
                                                            t.second);
        EXPECT_NEAR(px[0], expect_tau, std::max(1e-4f, expect_tau * 1e-3f))
            << "切片 " << k << " 的 τ";
        // S = color·(1 − e^{−τ})：内散射色就是用户给的颜色，不吃光照。
        const float expect_s[3] = {color.r * cover, color.g * cover, color.b * cover};
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(px[1 + c], expect_s[c], std::max(1e-4f, expect_s[c] * 1e-3f))
                << "切片 " << k << " 的 S[" << c << "]";
        }
    }
}

// ── 用例 2：既无雾团也无远景雾 ⇒ 场无效（零开销，而非"跑一趟全零"）──
TEST(DistanceFogGpuTest, NoFogNoField) {
    Fixture fx;
    fx.Setup();

    const jpov::Camera cam = MakeCamera();
    const jpov::FogParams params = MakeParams();
    float mvp[16];
    jpov::Primitives3DRenderer::BuildMVP(cam, kViewW, kViewH, mvp);

    jpov::FogField field;
    const jpov::FogLighting light;
    const std::vector<jpov::PointFog> no_fogs;
    const std::vector<jpov::PointLight> no_lights;
    fx.fog.Build(no_fogs, no_lights, 0, cam, mvp, kViewW, kViewH, params, light,
                 /*distance_fog=*/nullptr, std::nullopt, &field);
    EXPECT_FALSE(field.valid());
    EXPECT_EQ(field.scatter_tex, 0u);

    // enabled=false 与 nullptr 等价（调用方不必先判）。
    jpov::DistanceFog off;
    off.enabled = false;
    off.size = {10.0f, 10.0f, 10.0f};
    off.sigma = 1.0f;
    fx.fog.Build(no_fogs, no_lights, 0, cam, mvp, kViewW, kViewH, params, light, &off,
                 std::nullopt, &field);
    EXPECT_FALSE(field.valid());
}

// ── 用例 3：指数软边 —— 壳区切片被剖面衰减，盒外恒 0 ──
//
// 几何刻意做成「薄板 + 视线严格沿 -z」：viewport 64、tile_px 64 ⇒ 只有 **1 个 tile
// 柱**，它的中心射线就是画面正中央那条（NDC 0,0）= 相机正前方 ⇒ 沿视线的距离与 z 完全
// 一致，不必为 tile 中心射线的倾斜做修正。
//
// 沿 -z 依次是：11–13 m 壳 → 13–17 m 内盒（w = 1）→ 17–19 m 壳 → 19 m 之后无场。
// 该深度上切片厚 ≈ 0.4 m ⇒ 两个壳区各覆盖约 5 个切片中点。
// 判据：壳区切片数 ≥ 5（把它们判成 w=1 就是硬边 ⇒ 红），且最深处 w < 0.5。
TEST(DistanceFogGpuTest, ExponentialShellSoftensEdge) {
    Fixture fx;
    fx.Setup();

    const float sigma = 0.02f;
    jpov::DistanceFog dist;
    dist.enabled = true;
    dist.center = {0.0f, 0.0f, -5.0f};
    dist.size = {800.0f, 800.0f, 4.0f};    // 内盒 z ∈ [-7,-3] ⇒ 距相机 13–17 m
    dist.radius = 2.0f;                    // 容器 z ∈ [-9,-1] ⇒ 11–19 m
    dist.sigma = sigma;
    dist.color = {1.0f, 1.0f, 1.0f, 1.0f};
    dist.attenuation = jpov::FogAttenuation::kExponential;

    jpov::Camera cam = MakeCamera();
    jpov::FogParams params = MakeParams();
    params.tile_px = kViewW;   // 1 个 tile 柱 ⇒ 中心射线 = 正前方
    float mvp[16];
    jpov::Primitives3DRenderer::BuildMVP(cam, kViewW, kViewH, mvp);

    jpov::FogField field;
    const jpov::FogLighting light;
    const std::vector<jpov::PointFog> no_fogs;
    const std::vector<jpov::PointLight> no_lights;
    fx.fog.Build(no_fogs, no_lights, 0, cam, mvp, kViewW, kViewH, params, light, &dist,
                 std::nullopt, &field);
    ASSERT_TRUE(field.valid());
    ASSERT_EQ(field.grid_cols, 1);
    ASSERT_EQ(field.grid_rows, 1);

    const float growth = std::pow(kZFar / kZNear, 1.0f / static_cast<float>(kNz));
    float prev_tau = 0.0f;
    float zk = kZNear;
    int n_inner = 0;
    int n_shell = 0;
    int n_outside = 0;
    float min_shell_w = 1.0f;
    for (int k = 0; k < kNz; ++k) {
        const float zk1 = zk * growth;
        const std::pair<int, int> t = fx.SliceTexel(field, 0, 0, k);
        const std::array<float, 4> px = fx.ReadScatterTexel(field.scatter_tex, t.first,
                                                            t.second);
        const float dtau = px[0] - prev_tau;
        const float full = sigma * (zk1 - zk);   // 全段都在盒内时的增量（w = 1）
        const float z_mid = 0.5f * (zk + zk1);
        // 按「整段落在哪」分类（不能用中点：跨面切片的中点仍可能在密区，但段被裁过）。
        // 沿 -z：11–13 壳 / 13–17 内盒 / 17–19 壳 / ≥19 无场。
        const bool fully_out = (zk >= 19.0f);
        const bool fully_inner = (zk >= 13.0f && zk1 <= 17.0f);
        const bool fully_shell = (zk >= 11.0f && zk1 <= 13.0f) ||
                                 (zk >= 17.0f && zk1 <= 19.0f);
        if (fully_out) {
            EXPECT_EQ(dtau, 0.0f) << "盒外切片 " << k << "（z≈" << z_mid << "）";
            ++n_outside;
        } else if (fully_inner) {
            EXPECT_NEAR(dtau, full, std::max(1e-5f, full * 2e-3f)) << "内盒切片 " << k;
            ++n_inner;
        } else if (fully_shell) {
            // 壳区：剖面衰减 ⇒ 严格 0 < w < 1（硬边会给出 w = 1 ⇒ 这里红）。
            const float w = dtau / full;
            EXPECT_GT(w, 0.0f);
            EXPECT_LT(w, 1.0f) << "壳区切片 " << k << "（z≈" << z_mid << "）没有被衰减";
            min_shell_w = std::min(min_shell_w, w);
            ++n_shell;
        } else {
            // 跨盒面的切片：被裁过，增量只可能在 [0, full] 之间。
            EXPECT_GE(dtau, 0.0f);
            EXPECT_LE(dtau, full * 1.001f);
        }
        prev_tau = px[0];
        zk = zk1;
    }
    EXPECT_GT(n_inner, 5) << "内盒覆盖的切片太少（测试几何不自洽）";
    EXPECT_GT(n_outside, 50) << "盒外覆盖的切片太少（测试几何不自洽）";
    // 两个壳区各 2 m、切片厚 ~0.4 m ⇒ 各约 5 片"整段在壳内"。硬边时 w 恒 1 ⇒ 红。
    EXPECT_GE(n_shell, 4) << "壳区切片太少 ⇒ 剖面没生效（等于硬边）";
    EXPECT_LT(min_shell_w, 0.5f) << "壳区最深处仍未明显衰减 ⇒ 剖面没生效";
}

// ── 用例 4：水面以下不算雾（雾只在水上）──
//
// 相机在 (0,0,10)、水平面（下界）y = −5、视线**斜向下** 45°（target = (0,−10,0)）。
// 视线在 t = 5/sin45° = 7.071 m 处穿过水面 —— 再往下的那一截不能有雾。
// 期望：最后一片切片的累积 τ = σ·(7.071 − z_near)，而不是 σ·(z_far − z_near)。
//
// ⚠️ 这条锁的是**裁剪方向**（写成 `y ≤ floor` 就变成"只算水下"，肉眼看图很难发现）。
// 相机本身在水下的情况由 Renderer 拒绝注入（不在这个函数里），另见 DistanceFog 头注释。
TEST(DistanceFogGpuTest, FloorClipsBelowWater) {
    Fixture fx;
    fx.Setup();

    const float sigma = 0.02f;
    const jpov::Color color = {0.4f, 0.5f, 0.7f, 1.0f};
    const jpov::DistanceFog dist = MakeEnclosingFog(sigma, color);
    jpov::Camera cam = MakeCamera();
    cam.target = {0.0f, -10.0f, 0.0f};   // 视线 = (0,−1,−1)/√2
    jpov::FogParams params = MakeParams();
    params.tile_px = kViewW;             // 1 个 tile 柱 ⇒ 中心射线 = 相机正前方
    float mvp[16];
    jpov::Primitives3DRenderer::BuildMVP(cam, kViewW, kViewH, mvp);

    const jpov::FogLighting light;
    const std::vector<jpov::PointFog> no_fogs;
    const std::vector<jpov::PointLight> no_lights;
    const float floor_y = -5.0f;
    const float t_cross = 5.0f / std::sin(3.14159265358979323846f / 4.0f);   // ≈ 7.0711
    const float growth = std::pow(kZFar / kZNear, 1.0f / static_cast<float>(kNz));

    // (a) 有下界：雾只累积到水面（t_cross）。
    jpov::FogField clipped;
    fx.fog.Build(no_fogs, no_lights, 0, cam, mvp, kViewW, kViewH, params, light, &dist,
                 /*fog_floor_y=*/floor_y, &clipped);
    ASSERT_TRUE(clipped.valid());
    ASSERT_EQ(clipped.grid_cols, 1);
    const std::pair<int, int> tc = fx.SliceTexel(clipped, 0, 0, clipped.nz - 1);
    const std::array<float, 4> pc = fx.ReadScatterTexel(clipped.scatter_tex, tc.first,
                                                       tc.second);
    const float expect_clipped = sigma * (t_cross - kZNear);
    EXPECT_NEAR(pc[0], expect_clipped, std::max(1e-3f, expect_clipped * 2e-2f))
        << "水下那一截不该有雾（τ 应停在水面处）";
    EXPECT_LT(pc[0], sigma * (kZFar - kZNear) * 0.5f) << "τ 没被水面截断";

    // (b) 无下界：一直累积到 z_far（对照，证明 (a) 的变化来自裁剪而不是别的东西）。
    jpov::FogField full;
    fx.fog.Build(no_fogs, no_lights, 0, cam, mvp, kViewW, kViewH, params, light, &dist,
                 std::nullopt, &full);
    ASSERT_TRUE(full.valid());
    const std::pair<int, int> tf = fx.SliceTexel(full, 0, 0, full.nz - 1);
    const std::array<float, 4> pf = fx.ReadScatterTexel(full.scatter_tex, tf.first,
                                                        tf.second);
    const float expect_full = sigma * (kZFar - kZNear);
    EXPECT_NEAR(pf[0], expect_full, std::max(1e-3f, expect_full * 1e-3f));
    EXPECT_GT(pf[0], pc[0] * 4.0f) << "有无下界必须差出量级（否则裁剪根本没生效）";
    (void)growth;
}

}  // namespace

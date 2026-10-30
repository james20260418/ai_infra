// JPOV SSR GL-free 算法单测 —— 整套 SSR 仅有的两条"公理"+ 分段并行的等价性
//
// 为什么这层测试最重要：SSR 算错了**不会崩**，只会得到"倒影偏半格 / 薄栏杆不漏 /
// 掠射时出现横带"这类在图上极难归因的症状。这两条公理（1/d 对屏幕弧长线性、
// 体素柱的区间重叠判据）和"分段取 min ≡ 单线程 DDA"这三件事，必须在 CPU 上钉死。
//
// 每条断言都配了**负向对照**（在同一个 TEST 里顺手把错误实现算一遍，断言它必须错）：
// 只断言"对的结果"很容易写出恒真断言，那样的测试跑一百次也发现不了问题。
//
// 覆盖：
//   1. WLinearInSigma          1/d 对屏幕弧长严格线性（负向：mix(d) 在掠射处误差 > 1 倍）
//   2. VoxelPredicate          厚墙 / 薄栏杆 / 轮廓跳变 / 闭区间边界（负向：首次跨越即命中）
//   3. SlabCoversWholeSlab     每段步数上限够走完满段（负向：步数砍到 1 就漏）
//   4. S_MidLandsInHitCell     从 s 反推的格 == 命中格
//   5. SlabsEqualSingleThread  分段 + min ≡ 单线程 DDA（同命中格）
//   6. MinPicksEarliest        两处都命中时必须取**早**的那个（负向：取 max 会拿到远的）
//   7. DerivedParams            c = N_px·θ_px / L / steps / 浪费告警阈值
//   8. DegenerateInputs         near→0 的 NaN 守卫、法线越界归一化、近平面裁剪、出屏

#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/src/ssr/ssr_math.h"

namespace {

using jpov::ssr::BuildScreenRay;
using jpov::ssr::CellAtSigma;
using jpov::ssr::CellHit;
using jpov::ssr::CellThickness;
using jpov::ssr::ClipRayLength;
using jpov::ssr::DepthFromWindowDepth;
using jpov::ssr::DeriveParams;
using jpov::ssr::DerivedParams;
using jpov::ssr::HitFacesRay;
using jpov::ssr::kNoHit;
using jpov::ssr::MarchSegment;
using jpov::ssr::MarchSlabs;
using jpov::ssr::MarchWholeRay;
using jpov::ssr::PosAt;
using jpov::ssr::ReconstructNormal;
using jpov::ssr::ReflectDir;
using jpov::ssr::ScreenRay;
using jpov::ssr::Vec2;
using jpov::ssr::Vec3;
using jpov::ssr::ViewPointAtUv;
using jpov::ssr::WAt;

// 一台"正常"相机：near 0.1 / far 1000 / 垂直 FOV 60° / 16:9。
struct Cam {
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    float tan_half = 0.57735027f;   // tan(30°)
    float aspect = 16.0f / 9.0f;
    int width = 320;
    int height = 180;
};

// 视空间点 → 屏幕像素坐标（针孔投影，与 BuildScreenRay 同一份定义）。
Vec2 ProjectToScreen(const Vec3& x, const Cam& cam) {
    const float d = -x.z;
    const float nx = x.x / (d * cam.aspect * cam.tan_half);
    const float ny = x.y / (d * cam.tan_half);
    return Vec2{(nx * 0.5f + 0.5f) * static_cast<float>(cam.width),
                (ny * 0.5f + 0.5f) * static_cast<float>(cam.height)};
}

// 走完 ①反构 → ②裁剪 的全流程，并把中间量（P / R / 长度）一并带出来给测试用。
struct RaySetup {
    Vec3 p;
    Vec3 r;
    float len = 0.0f;
    ScreenRay ray;
    bool ok = false;
};

RaySetup MakeRaySetup(const Cam& cam, float u, float v, const Vec3& normal, float d_surf,
                      float max_dist_m) {
    RaySetup s;
    s.p = ViewPointAtUv(u, v, d_surf, cam.tan_half, cam.aspect);
    s.r = ReflectDir(s.p, normal);
    s.len = ClipRayLength(s.p, s.r, cam.near_plane, cam.far_plane, max_dist_m);
    s.ok = s.len > 0.0f &&
           BuildScreenRay(s.p, s.r, s.len, cam.tan_half, cam.aspect, cam.width, cam.height,
                          &s.ray);
    return s;
}

RaySetup MakeRaySetup(const Cam& cam, float u, float v, const Vec3& normal) {
    return MakeRaySetup(cam, u, v, normal, /*d_surf=*/20.0f, /*max_dist_m=*/900.0f);
}

// ───────────────────────── 1. 1/d 对 σ 严格线性 ─────────────────────────
//
// 论点（§3.1）：屏幕位置对 σ 线性、w = 1/d 对屏幕坐标仿射 ⇒ w(σ) 是一条**直线**。
// 检验：沿 3D 线段取点（只用**正向**投影算 σ，不碰 w 的插值），再拿 WAt 反算 w，
// 与 1/(−X.z) 对比。负向：在 d 空间线性插值，掠射处误差必须很大。
TEST(SsrMath, WLinearInSigma) {
    std::mt19937 rng(20261029u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Cam cam;

    double worst_rel = 0.0;
    double worst_naive = 0.0;
    int samples = 0;
    for (int trial = 0; trial < 400; ++trial) {
        // 法线偏上（地板镜面）⇒ 自然出现掠射（射线贴着地面飞，d 跨越几个数量级）。
        const Vec3 n = ReconstructNormal(0.30f * (uni(rng) - 0.5f), 0.45f + 0.55f * uni(rng));
        const RaySetup s = MakeRaySetup(cam, 0.15f + 0.7f * uni(rng), 0.15f + 0.7f * uni(rng), n);
        // 只要**有屏幕足迹**的射线：屏幕足迹 < 16 px 的射线是"几乎正对视线方向飞出去"的
        // 那种，投影差异只剩几个像素，λ 在 float32 下本来就是病态的（实测 2e-3），
        // 且这种射线在真实场景里也拿不到任何屏幕数据（会被判 a=0）。见下面 worst_rel 说明。
        if (!s.ok || s.ray.span < 16.0f) {
            continue;
        }
        for (int k = 1; k <= 8; ++k) {
            const float t = static_cast<float>(k) / 9.0f;
            const Vec3 x = s.p + s.r * (s.len * t);   // 3D 线段上的点
            const float d = -x.z;
            ASSERT_GT(d, 0.0f);
            const Vec2 q = ProjectToScreen(x, cam);
            const float lam = jpov::ssr::Length(q - s.ray.s0) / s.ray.span;
            ASSERT_GE(lam, -1.0e-4f);
            ASSERT_LE(lam, 1.0f + 1.0e-4f);

            const float w = WAt(s.ray, lam * s.ray.span);
            const double rel = std::fabs(w - 1.0f / d) / (1.0f / static_cast<double>(d));
            worst_rel = std::max(worst_rel, rel);
            ++samples;

            // 负向：同样的 λ 用 d 线性插值。
            const float d0 = 1.0f / s.ray.w0;
            const float d1 = 1.0f / s.ray.w1;
            const float d_naive = d0 + (d1 - d0) * lam;
            worst_naive =
                std::max(worst_naive, std::fabs(d_naive - d) / static_cast<double>(d));
        }
    }
    ASSERT_GT(samples, 1000);
    // 容差说明：float32 下 w 是两个 O(1/d0) 量的差（d 跨 20→900 倍），条件数约 20~40，
    // 实测 worst ≈ 1.2e-4（span≥128 时）~1.5e-4（span≥16）⇒ 取 5e-4 有 3 倍余量。
    // 真正的实现错误（在 d 空间插值）误差量级是 1e1（下面那条负向），差 5 个数量级。
    EXPECT_LT(worst_rel, 5.0e-4) << "w = 1/d 沿屏幕弧长的线性插值不成立（实现有误）";
    // 负向对照的意义：证明样本里确有掠射，否则"线性"这件事根本没被检验到。
    EXPECT_GT(worst_naive, 1.0) << "样本里没有掠射情形，本测试的负向对照失效";
}

// ───────────────────────── 2. 体素柱区间判据 ─────────────────────────

TEST(SsrMath, VoxelPredicate) {
    // (a) 厚墙：200 m 外的墙，射线每格穿透 1.2 m，T = 0.5 m —— 必须命中。
    //     这正是 §4.2 的决定性证据：双线性判据（是否"留在"厚度带内）会判成"穿过"。
    {
        const float z = 200.0f;
        const float t = 0.5f;
        EXPECT_TRUE(CellHit(1.0f / 199.8f, 1.0f / 201.0f, z, t))
            << "区间 [199.8,201.0] ∩ [200,200.5] 非空，必须命中";
        const float g_exit = 201.0f - 200.0f;   // 每格穿透量 1.0 m
        EXPECT_GT(g_exit, t) << "双线性那套判据在同输入下会判'穿过'——这就是放弃它的理由";
    }
    // (b) 薄栏杆：射线整段在柱子后方 ⇒ 必须放行（负向：首次跨越即命中会误判）。
    {
        const float z = 50.0f;
        const float t = 0.5f;
        const float w_in = 1.0f / 60.0f;
        const float w_out = 1.0f / 61.0f;
        EXPECT_FALSE(CellHit(w_in, w_out, z, t)) << "整段在柱子后方必须放行";
        const bool naive_hit = (1.0f / std::min(w_in, w_out)) >= z;
        EXPECT_TRUE(naive_hit) << "负向对照失效：'首次跨越即命中'在这里居然也判对了";
    }
    // (c) 轮廓跳变：同一射线区间对前格命中、对远格放行。
    {
        const float w_in = 1.0f / 1.00f;
        const float w_out = 1.0f / 1.05f;
        EXPECT_TRUE(CellHit(w_in, w_out, /*z=*/1.0f, /*t=*/0.02f));
        EXPECT_FALSE(CellHit(w_in, w_out, /*z=*/1000.0f, /*t=*/10.0f));
    }
    // (d) 边界闭区间：d_hi == z、d_lo == z+T 都算命中（≥/≤ 而非 >/<）。
    {
        EXPECT_TRUE(CellHit(1.0f / 1.0f, 1.0f / 2.0f, /*z=*/2.0f, /*t=*/0.0f))
            << "射线恰好抵达前表面应算命中";
        EXPECT_TRUE(CellHit(1.0f / 3.0f, 1.0f / 4.0f, /*z=*/2.0f, /*t=*/1.0f))
            << "射线恰好触及后表面应算命中";
    }
    // (e) 厚度随本格深度，且被 min_thickness 兜住。
    {
        const float c = 0.01f;
        EXPECT_FLOAT_EQ(CellThickness(200.0f, c, 1.0e-4f), 2.0f);
        EXPECT_FLOAT_EQ(CellThickness(0.001f, c, 1.0e-4f), 1.0e-4f) << "min_thickness 必须生效";
    }
}

// ───────────────────────── 3. 每段步数上限够走完满段 ─────────────────────────
//
// 段长 L 在**对角线**最坏情况下跨 ceil(√2·L)+1 个格子。上限少一步 ⇒ 段尾那一格永远扫不到，
// 症状是"倒影在每段接缝处缺一格"，且只在特定角度出现（最难查的一类）。
TEST(SsrMath, SlabCoversWholeSlab) {
    const Cam cam;
    const float kMinThickness = 1.0e-4f;
    const int kMaxRayPixels = 160;
    int checked = 0;
    int missed = 0;
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d =
            DeriveParams(tile, kMaxRayPixels, /*N_px=*/10.0f, cam.tan_half, cam.height);
        for (int trial = 0; trial < 150; ++trial) {
            std::mt19937 rng(1u + static_cast<unsigned>(trial));
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            const Vec3 n = ReconstructNormal(0.30f * (uni(rng) - 0.5f), 0.5f + 0.5f * uni(rng));
            const RaySetup s = MakeRaySetup(cam, 0.1f + 0.8f * uni(rng), 0.1f + 0.8f * uni(rng), n);
            if (!s.ok || s.ray.sigma_exit < d.slab_pixels * 1.5f) {
                continue;
            }
            for (int lane = 0; lane < d.lanes; ++lane) {
                const float begin = std::max(d.slab_pixels * static_cast<float>(lane),
                                             s.ray.sigma_enter);
                const float end = std::min(d.slab_pixels * static_cast<float>(lane + 1),
                                           s.ray.sigma_exit);
                if (!(end > begin + 0.5f)) {
                    continue;
                }
                // 只把"段尾最后一格"设成该处射线的视深，其余全是天空（far）
                // ⇒ 命中只可能来自那一格；扫不到就说明步数上限不足。
                const float tail_sigma = end - 1.0e-3f;
                const Vec2 tail = PosAt(s.ray, tail_sigma);
                const int tx = static_cast<int>(std::floor(tail.x));
                const int ty = static_cast<int>(std::floor(tail.y));
                auto depth_at = [&](int x, int y) -> float {
                    return (x == tx && y == ty) ? (1.0f / WAt(s.ray, tail_sigma))
                                                : cam.far_plane;
                };
                const float hit =
                    MarchSegment(s.ray, begin, end, d.steps_per_lane,
                                 d.slab_pixels * static_cast<float>(lane) <= 0.0f,
                                 d.thickness_coefficient, kMinThickness, depth_at, cam.width,
                                 cam.height, nullptr, nullptr);
                ++checked;
                if (hit >= kNoHit) {
                    ++missed;
                }
            }
        }
    }
    EXPECT_GT(checked, 500) << "样本太少，覆盖不到各种角度";
    EXPECT_EQ(missed, 0) << "段尾那一格被步数上限截断了（steps_per_lane 偏小）";

    // 负向：步数砍到 1 ⇒ 大部分段扫不到段尾。
    {
        int full = 0;
        int short_hits = 0;
        for (int trial = 0; trial < 200; ++trial) {
            std::mt19937 rng(7u + static_cast<unsigned>(trial));
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            const Vec3 n = ReconstructNormal(0.2f * (uni(rng) - 0.5f), 0.6f + 0.4f * uni(rng));
            const RaySetup s = MakeRaySetup(cam, 0.2f + 0.6f * uni(rng), 0.2f + 0.6f * uni(rng), n);
            const DerivedParams d = DeriveParams(4, kMaxRayPixels, 10.0f, cam.tan_half, cam.height);
            if (!s.ok || s.ray.sigma_exit < d.slab_pixels * 1.5f) {
                continue;
            }
            const float begin = s.ray.sigma_enter;
            const float end = std::min(d.slab_pixels, s.ray.sigma_exit);
            const float tail_sigma = end - 1.0e-3f;
            const Vec2 tail = PosAt(s.ray, tail_sigma);
            const int tx = static_cast<int>(std::floor(tail.x));
            const int ty = static_cast<int>(std::floor(tail.y));
            auto depth_at = [&](int x, int y) -> float {
                return (x == tx && y == ty) ? (1.0f / WAt(s.ray, tail_sigma)) : cam.far_plane;
            };
            if (MarchSegment(s.ray, begin, end, d.steps_per_lane, begin <= 0.0f,
                             d.thickness_coefficient, kMinThickness, depth_at, cam.width,
                             cam.height, nullptr, nullptr) < kNoHit) {
                ++full;
            }
            if (MarchSegment(s.ray, begin, end, 1, begin <= 0.0f, d.thickness_coefficient,
                             kMinThickness, depth_at, cam.width, cam.height, nullptr,
                             nullptr) < kNoHit) {
                ++short_hits;
            }
        }
        EXPECT_GT(full, 100) << "满步数下段尾几乎从不命中 ⇒ 上面的断言没覆盖真实情形";
        EXPECT_LT(short_hits, full / 4) << "步数砍到 1 仍能命中大半 ⇒ 步数上限没起作用";
    }
}

// ───────────────────────── 4. s → 格 必须落回命中格 ─────────────────────────

// 射线在格 (cx,cy) 内的 σ 区间（与该格正方形的交）。用于独立校验 DDA 的 σ 记账。
bool CellSigmaRange(const ScreenRay& ray, int cx, int cy, float* lo, float* hi) {
    float t0 = 0.0f;
    float t1 = ray.span;
    const float box_lo[2] = {static_cast<float>(cx), static_cast<float>(cy)};
    const float box_hi[2] = {static_cast<float>(cx + 1), static_cast<float>(cy + 1)};
    const float p0[2] = {ray.s0.x, ray.s0.y};
    const float dir[2] = {ray.dir.x, ray.dir.y};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::fabs(dir[axis]) < 1.0e-9f) {
            if (p0[axis] < box_lo[axis] || p0[axis] > box_hi[axis]) {
                return false;
            }
            continue;
        }
        float ta = (box_lo[axis] - p0[axis]) / dir[axis];
        float tb = (box_hi[axis] - p0[axis]) / dir[axis];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (!(t1 > t0)) {
        return false;
    }
    *lo = t0;
    *hi = t1;
    return true;
}

// 两个场景各测一遍 s → 格 的反推：
//   场 A：整屏同一深度 20 m ⇒ 每条射线都在起点格之后的**第一格**命中（方向覆盖全）；
//   场 B：下半屏 20 m / 上半屏 200 m ⇒ 向上的射线在 d≈200 处命中，s 落在很远的格里。
TEST(SsrMath, S_MidLandsInHitCell) {
    const Cam cam;
    std::mt19937 rng(99u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    auto field_flat = [&](int x, int y) -> float {
        (void)x;
        (void)y;
        return 20.0f;
    };
    auto field_two_layers = [&](int x, int y) -> float {
        (void)x;
        return (y >= cam.height / 2) ? 600.0f : 20.0f;
    };

    int checked = 0;
    int bad_cell = 0;
    int bad_range = 0;
    int late_hits = 0;
    for (int trial = 0; trial < 900; ++trial) {
        // 一半样本用"地板式"法线（ny ≈ 0.95，z ≈ 0.3）：反射射线几乎贴着屏幕飞、
        // d 增长很慢 ⇒ 能跑出几十~几百像素的长路径，把"远距离命中"也覆盖到。
        // 另一半是一般法线（射线很快离开屏幕，短路径）。
        const Vec3 n = (trial % 2 == 0)
                           ? ReconstructNormal(0.35f * (uni(rng) - 0.5f), 0.2f + 0.8f * uni(rng))
                           : ReconstructNormal(0.10f * (uni(rng) - 0.5f),
                                               0.90f + 0.099f * uni(rng));
        const RaySetup s = MakeRaySetup(cam, 0.05f + 0.9f * uni(rng), 0.05f + 0.9f * uni(rng), n);
        if (!s.ok) {
            continue;
        }
        for (int variant = 0; variant < 2; ++variant) {
            const DerivedParams d = DeriveParams(4, 300, 10.0f, cam.tan_half, cam.height);
            int hx = -1;
            int hy = -1;
            const float hit =
                (variant == 0)
                    ? MarchSlabs(s.ray, d, 1.0e-4f, field_flat, cam.width, cam.height, &hx, &hy)
                    : MarchSlabs(s.ray, d, 1.0e-4f, field_two_layers, cam.width, cam.height, &hx,
                                 &hy);
            if (hit >= kNoHit) {
                continue;
            }
            ++checked;
            if (hit > 30.0f) {
                ++late_hits;
            }
            int cx = -1;
            int cy = -1;
            CellAtSigma(s.ray, hit, &cx, &cy);
            if (cx != hx || cy != hy) {
                ++bad_cell;
                continue;
            }
            // 独立校验：报告的 s 必须落在"命中格的 σ 区间"内（DDA 的 σ 记账对不对）。
            float lo = 0.0f;
            float hi = 0.0f;
            if (!CellSigmaRange(s.ray, hx, hy, &lo, &hi) || hit < lo - 1.0e-3f ||
                hit > hi + 1.0e-3f) {
                ++bad_range;
            }
        }
    }
    EXPECT_GT(checked, 400) << "样本里命中太少，测不到反推";
    EXPECT_GT(late_hits, 20) << "几乎全是紧贴着起点格的命中 ⇒ 长路径的记账没被覆盖到";
    EXPECT_EQ(bad_cell, 0) << "s_mid 反推出的格与命中格不一致 ⇒ 取色会取错格子";
    EXPECT_EQ(bad_range, 0) << "报告的 s 不在命中格的 σ 区间内 ⇒ DDA 的 σ 记账有误";
}

// ───────────────────────── 5. 分段 + min ≡ 单线程 DDA ─────────────────────────
//
// 整个加速方案的全部正确性依据：T² 个 lane 各报本段最早命中、取 min，
// 必须与"一条线走到底"得到**同一个命中格**（格内中点会因段边界而略不同，但格子必须相同
// —— 取色只依赖格子）。
TEST(SsrMath, SlabsEqualSingleThread) {
    const Cam cam;
    std::mt19937 rng(4242u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    std::vector<std::pair<int, float>> columns;   // 竖向"墙"分层
    for (int x = 0; x < cam.width; x += 17) {
        columns.emplace_back(x, 3.0f + 400.0f * uni(rng));
    }
    auto depth_at = [&](int x, int y) -> float {
        (void)y;
        float z = cam.far_plane;
        for (const std::pair<int, float>& c : columns) {
            if (x >= c.first && x < c.first + 17) {
                z = c.second;
            }
        }
        return z;
    };

    int compared = 0;
    int mismatched = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const Vec3 n = ReconstructNormal(0.30f * (uni(rng) - 0.5f), 0.3f + 0.7f * uni(rng));
        const RaySetup s = MakeRaySetup(cam, 0.05f + 0.9f * uni(rng), 0.05f + 0.9f * uni(rng), n);
        if (!s.ok) {
            continue;
        }
        int ref_x = -1;
        int ref_y = -1;
        const float s_ref = MarchWholeRay(s.ray, 0.01f, 1.0e-4f, 100000, depth_at, cam.width,
                                          cam.height, &ref_x, &ref_y);
        for (int tile : {1, 2, 4, 8}) {
            const DerivedParams d = DeriveParams(tile, 220, 10.0f, cam.tan_half, cam.height);
            int sx = -1;
            int sy = -1;
            const float hit =
                MarchSlabs(s.ray, d, 1.0e-4f, depth_at, cam.width, cam.height, &sx, &sy);
            ++compared;
            const bool both_miss = (s_ref >= kNoHit) && (hit >= kNoHit);
            const bool same_cell =
                (s_ref < kNoHit) && (hit < kNoHit) && sx == ref_x && sy == ref_y;
            if (!both_miss && !same_cell) {
                ++mismatched;
            }
        }
    }
    EXPECT_GT(compared, 800) << "样本太少";
    EXPECT_EQ(mismatched, 0) << "分段取 min 与单线程 DDA 命中了不同的格子（方案失效）";
}

// ───────────────────────── 6. min 必须取**最早**那处命中 ─────────────────────────
//
// 手工构造一条水平射线 + 恒深度场，沿路径放两处命中格。min 必须拿近的（x=20），
// 而"取 max"会拿到远的（x=60）⇒ 这条断言直接证明归约方向不能反。
TEST(SsrMath, MinPicksEarliest) {
    ScreenRay ray;
    ray.s0 = Vec2{0.5f, 10.5f};
    ray.dir = Vec2{1.0f, 0.0f};
    ray.span = 100.0f;
    ray.w0 = 1.0f / 10.0f;   // 恒深度 10 m ⇒ w(σ) 恒为 0.1
    ray.w1 = ray.w0;
    ray.sigma_enter = 0.0f;
    ray.sigma_exit = 100.0f;
    ray.exits_screen = false;

    auto depth_at = [](int x, int y) -> float {
        (void)y;
        if (x == 20 || x == 60) {
            return 10.0f;
        }
        return 1000.0f;
    };

    int hx = -1;
    int hy = -1;
    const float hit = MarchWholeRay(ray, 0.01f, 1.0e-4f, 1000, depth_at, 320, 180, &hx, &hy);
    ASSERT_LT(hit, kNoHit);
    EXPECT_EQ(hx, 20) << "必须命中更近的那处（min 方向反了就会拿到 x=60）";

    // 分段版同样取近的。
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d = DeriveParams(tile, 160, 10.0f, 0.57735027f, 180);
        int sx = -1;
        int sy = -1;
        const float s = MarchSlabs(ray, d, 1.0e-4f, depth_at, 320, 180, &sx, &sy);
        ASSERT_LT(s, kNoHit) << "tile=" << tile;
        EXPECT_EQ(sx, 20) << "tile=" << tile << " 取到了较远的命中格（min 被写成了 max）";
    }

    // 负向：取 max 会拿到 x=60 —— 显式算一遍，证明上面的断言有牙。
    float latest = -1.0f;
    int lx = -1;
    for (int lane = 0; lane < 16; ++lane) {
        const DerivedParams d = DeriveParams(4, 160, 10.0f, 0.57735027f, 180);
        const float begin = d.slab_pixels * static_cast<float>(lane);
        const float end = begin + d.slab_pixels;
        int cx = -1;
        int cy = -1;
        const float s = MarchSegment(ray, begin, end, d.steps_per_lane, begin <= 0.0f,
                                     d.thickness_coefficient, 1.0e-4f, depth_at, 320, 180, &cx,
                                     &cy);
        if (s < kNoHit && s > latest) {
            latest = s;
            lx = cx;
        }
    }
    EXPECT_EQ(lx, 60) << "取 max 应得到较远的 x=60 —— 负向对照失效";
}

// ───────────────────────── 7. 派生参数 ─────────────────────────

// ───────────────────────── 7. 命中面朝向（背面命中不算）─────────────────────────
//
// 2026-10-30 Danis 实拍发现：体素柱/桌子这种厚物体，镜子里的射线是从它**背面**穿过去的，
// 旧判据却把**正面**（带高光）的像素当结果 ⇒ 倒影里桌子底下也在发光。
//
// 本测试用一个手搭的场景把两类命中分开（不经过相机反构，屏幕空间可控）：
//   rows  0..39  无几何（空格子，穿过去）
//   rows 40..59  “桌子顶面” z=20 m，法线**朝上** ⇒ 射线从下往上穿 ⇒ R·N > 0 ⇒ 必须拒
//   rows 60..79  无几何
//   rows 80..    “墙” z=20 m，法线**朝相机** ⇒ 射线扎进去 ⇒ R·N < 0 ⇒ 必须命中
// 三件事一起钉：① 旧行为确实先撞桌子（bug 的存在性）；② 加了朝向判据后跳过桌子、
// 命中后面的墙；③ 把墙的法线翻过来也必须拒（防"总是接受"的恒真式实现）。
ScreenRay MakeSyntheticRay() {
    ScreenRay ray;
    ray.s0 = Vec2{100.0f, 20.0f};
    ray.dir = Vec2{0.0f, 1.0f};
    ray.dir3 = Normalize(Vec3{0.0f, 0.35f, -0.94f});   // 向上 + 远离相机
    ray.span = 100.0f;
    ray.w0 = 1.0f / 20.0f;
    ray.w1 = 1.0f / 24.0f;   // 视深 20 m → 24 m，对 σ 线性（w = 1/d）
    ray.sigma_enter = 0.0f;
    ray.sigma_exit = 100.0f;
    ray.exits_screen = false;
    return ray;
}

TEST(SsrMath, BackfaceHitRejected) {
    const ScreenRay ray = MakeSyntheticRay();
    // c = 0.18 ⇒ T = 3.6 m（z=20）⇒ 射线在桌子/墙那两段确实落在柱体带内，
    // 所以"能不能命中"完全取决于朝向判据，不是厚度在挡。
    const float kThicknessC = 0.18f;
    const float kMinThickness = 1.0e-4f;

    auto depth_at = [](int /*x*/, int y) -> float {
        if (y >= 40 && y < 60) {
            return 20.0f;   // 桌子顶面
        }
        if (y >= 80 && y < 120) {
            return 20.0f;   // 墙
        }
        return jpov::ssr::kEmptyCell;
    };
    auto facing = [](int /*x*/, int y, const Vec3& d) -> bool {
        if (y >= 40 && y < 60) {
            return HitFacesRay(d, Vec3{0.0f, 1.0f, 0.0f});   // 法线朝上
        }
        if (y >= 80) {
            return HitFacesRay(d, Vec3{0.0f, 0.0f, 1.0f});   // 法线朝相机
        }
        return true;
    };

    // ① 旧行为（不判朝向）：先撞上桌子顶面 —— 这就是那个 bug。
    int old_x = -1;
    int old_y = -1;
    const float s_old = MarchWholeRay(ray, kThicknessC, kMinThickness, 200, depth_at, 320, 180,
                                      &old_x, &old_y);
    ASSERT_LT(s_old, kNoHit) << "旧判据应该在桌子段命中（否则本测试没造出 bug 场景）";
    EXPECT_GE(old_y, 40);
    EXPECT_LT(old_y, 60);

    // ② 加朝向判据：桌子段被跳过 ⇒ 命中后面的墙。
    int new_x = -1;
    int new_y = -1;
    const float s_new = MarchWholeRay(ray, kThicknessC, kMinThickness, 200, depth_at, 320, 180,
                                      &new_x, &new_y, facing);
    ASSERT_LT(s_new, kNoHit) << "墙是正面命中，不应被一起拒掉";
    EXPECT_GE(new_y, 80) << "背面命中（桌子顶面）没被跳过";
    EXPECT_GT(s_new, s_old) << "新命中必须比旧命中更远（跳到桌子后面去了）";

    // ③ 防恒真：把墙的法线翻到背面 ⇒ 整条射线应该无命中。
    auto facing_flipped = [](int /*x*/, int /*y*/, const Vec3& d) -> bool {
        return HitFacesRay(d, Vec3{0.0f, 0.0f, -1.0f});
    };
    EXPECT_GE(MarchWholeRay(ray, kThicknessC, kMinThickness, 200, depth_at, 320, 180, nullptr,
                            nullptr, facing_flipped),
              kNoHit)
        << "法线翻过来仍能命中 ⇒ 朝向判据是恒真的";
}

TEST(SsrMath, DerivedParams) {
    const Cam cam;
    const DerivedParams d =
        DeriveParams(/*tile=*/4, /*max_ray_pixels=*/400, /*N_px=*/10.0f, cam.tan_half, cam.height);
    EXPECT_EQ(d.lanes, 16);
    EXPECT_FLOAT_EQ(d.slab_pixels, 25.0f);
    EXPECT_EQ(d.steps_per_lane, static_cast<int>(std::ceil(1.41421356f * 25.0f)) + 3);
    const float expected_c = 10.0f * (2.0f * std::atan(cam.tan_half)) / 180.0f;
    EXPECT_NEAR(d.thickness_coefficient, expected_c, 1.0e-6f);
    EXPECT_FALSE(jpov::ssr::SlabTooShortForEfficiency(d)) << "25 px/段不该被告警";

    // 1/8 分辨率 + tile 8 + reach 40：L = 0.625 px ⇒ 必须被标记（只告警，不失败）。
    const DerivedParams bad = DeriveParams(8, 40, 10.0f, cam.tan_half, 135);
    EXPECT_FLOAT_EQ(bad.slab_pixels, 40.0f / 64.0f);
    EXPECT_TRUE(jpov::ssr::SlabTooShortForEfficiency(bad));

    // T=1 时 L 就是全部预算。
    const DerivedParams one = DeriveParams(1, 300, 10.0f, cam.tan_half, 1080);
    EXPECT_EQ(one.lanes, 1);
    EXPECT_FLOAT_EQ(one.slab_pixels, 300.0f);
}

// ───────────────────────── 8. 退化输入 ─────────────────────────

TEST(SsrMath, DegenerateInputs) {
    const Cam cam;

    // near ≈ 0 且 depth = 1.0（分母 0/0）：必须给有限正值，绝不能是 NaN/inf
    // —— NaN 会顺着后处理污染整帧。
    for (float near_p : {0.0f, 1.0e-9f}) {
        const float z = DepthFromWindowDepth(/*depth=*/1.0f, near_p, 1000.0f);
        EXPECT_TRUE(std::isfinite(z)) << "near=" << near_p << " 时深度反算出了 NaN/inf";
        EXPECT_GT(z, 0.0f);
    }
    // 正常相机：depth 0 / 1 必须是 near / far。
    EXPECT_NEAR(DepthFromWindowDepth(0.0f, 0.1f, 1000.0f), 0.1f, 1.0e-4f);
    // far 端：ndc = 1 时公式是 2fn/(2n) 的退化形式，float32 下实测 1000.244（相对 2.4e-4），
    // 不是实现问题（深度值越靠 far 分辨率越粗，这是透视深度缓冲的固有性质）。
    EXPECT_NEAR(DepthFromWindowDepth(1.0f, 0.1f, 1000.0f), 1000.0f, 1.0f);
    // 深度单调：越近的深度值越小。
    EXPECT_LT(DepthFromWindowDepth(0.2f, 0.1f, 1000.0f), DepthFromWindowDepth(0.6f, 0.1f, 1000.0f));

    // 法线 xy 长度 > 1（法线贴图/波浪扰动越界）必须归一化，否则 reflect 会把 R 拉长。
    const Vec3 n = ReconstructNormal(1.4f, 0.2f);
    EXPECT_NEAR(jpov::ssr::Length(n), 1.0f, 1.0e-6f);
    const Vec3 n0 = ReconstructNormal(0.0f, 0.0f);
    EXPECT_NEAR(n0.z, 1.0f, 1.0e-6f) << "清空的法线应退化成'平面朝相机'";

    // 朝相机走的射线必须裁到近平面之前（否则跨过 d = 0 的极点）。
    {
        const Vec3 p{0.0f, 0.0f, -50.0f};
        const Vec3 r{0.0f, 0.0f, 1.0f};
        EXPECT_NEAR(ClipRayLength(p, r, cam.near_plane, cam.far_plane, 500.0f),
                    50.0f - cam.near_plane, 1.0e-4f);
    }
    // 起点在屏幕外右侧、朝左走 ⇒ 裁剪后 sigma_enter > 0；整条在屏外 ⇒ 直接 false。
    {
        const Vec3 p{40.0f, 0.0f, -20.0f};
        const Vec3 r = jpov::ssr::Normalize(Vec3{-1.0f, 0.0f, -0.2f});
        const float len = ClipRayLength(p, r, cam.near_plane, cam.far_plane, 500.0f);
        ScreenRay ray;
        ASSERT_TRUE(
            BuildScreenRay(p, r, len, cam.tan_half, cam.aspect, cam.width, cam.height, &ray));
        EXPECT_GT(ray.sigma_enter, 0.0f) << "起点在屏外时 sigma_enter 必须 > 0";

        const Vec3 p_far{400.0f, 0.0f, -1.0f};
        const Vec3 r_far = jpov::ssr::Normalize(Vec3{1.0f, 0.0f, -0.01f});
        ScreenRay ray2;
        EXPECT_FALSE(BuildScreenRay(p_far, r_far, 10.0f, cam.tan_half, cam.aspect, cam.width,
                                    cam.height, &ray2));
    }
}

}  // namespace

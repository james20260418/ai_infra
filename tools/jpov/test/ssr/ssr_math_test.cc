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
using jpov::ssr::CellCrossesFront;
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
using jpov::ssr::UpsampleDepthAccept;
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

TEST(SsrMath, CrossesFrontPredicate) {
    // 判据只有一条：射线在这一格里**穿过了前表面**（z_in ≤ z_front ≤ z_out）。
    //
    // (a) 穿面 ⇒ 命中。射线在格内从 199.8 m 走到 201.0 m，面在 200 m。
    EXPECT_TRUE(CellCrossesFront(1.0f / 199.8f, 1.0f / 201.0f, 1.0f / 200.0f))
        << "射线跨过前表面必须命中";

    // (b) 面在射线终点之后 ⇒ 放行。
    EXPECT_FALSE(CellCrossesFront(1.0f / 60.0f, 1.0f / 61.0f, 1.0f / 50.0f))
        << "面比射线终点还远 ⇒ 不可能命中";

    // (c) 面比射线起点还近（射线已经在面后面/实体内）⇒ 放行。
    //     这一条替代了旧的"背面命中"判据：实体内部穿行时 z_in > z_front，自动不成立。
    EXPECT_FALSE(CellCrossesFront(1.0f / 10.0f, 1.0f / 12.0f, 1.0f / 6.0f))
        << "射线起点已在面后 ⇒ 不是正面命中";

    // (d) 空格（天空 / 无深度数据）⇒ 这里没有面。
    EXPECT_FALSE(CellCrossesFront(1.0f / 10.0f, 1.0f / 12.0f, 0.0f));

    // (e) 闭区间：恰好等于前端 / 后端都算命中（避免边界处漏一格）。
    EXPECT_TRUE(CellCrossesFront(1.0f / 2.0f, 1.0f / 4.0f, 1.0f / 2.0f));
    EXPECT_TRUE(CellCrossesFront(1.0f / 2.0f, 1.0f / 4.0f, 1.0f / 4.0f));

    // (f) 负向对照（这就是被删掉的那条魔法的病灶）：掠射时射线在一格里跨 30 m，
    //     旧代码把"体素厚度"抬到 30 m，于是 6 m 处的树被 30 m 的射线判成命中。
    //     新判据必须放行；把旧算式抄回来必须命中 —— 两边都断言，防止未来又把抬升加回来。
    {
        const float z_in = 30.0f;
        const float z_out = 60.0f;
        const float z_cell = 6.0f;
        const float w_in = 1.0f / z_in;
        const float w_out = 1.0f / z_out;
        const float w_cell = 1.0f / z_cell;
        EXPECT_FALSE(CellCrossesFront(w_in, w_out, w_cell))
            << "6 m 的面不该被 30→60 m 的射线命中";
        const float dz_cell = z_out - z_in;               // 旧的"分辨率下限"
        const float w_hi = 1.0f / (z_cell + dz_cell);     // thick_eff = max(thick, dz_cell)
        const bool old_hit =
            (std::min(w_in, w_out) <= w_cell) && (std::max(w_in, w_out) >= w_hi);
        EXPECT_TRUE(old_hit) << "负向对照失效：dz_cell 抬升在这里居然没误判";
    }
}

// ───────────────────────── 3. 每段步数上限够走完满段 ─────────────────────────
//
// lane 把射线的 [enter, exit] **等分**成 lanes 段。每段在对角线最坏情况下跨
// ceil(√2·L)+1 个格子；上限少一步 ⇒ 段尾那一格永远扫不到，症状是"倒影在每段接缝处
// 缺一格"，且只在特定角度出现（最难查的一类）。
TEST(SsrMath, StepsCoverLaneTail) {
    const Cam cam;
    int checked = 0;
    int missed = 0;
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d = DeriveParams(tile, cam.width, cam.height);
        for (int trial = 0; trial < 150; ++trial) {
            std::mt19937 rng(1u + static_cast<unsigned>(trial));
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            const Vec3 n = ReconstructNormal(0.30f * (uni(rng) - 0.5f), 0.5f + 0.5f * uni(rng));
            const RaySetup s = MakeRaySetup(cam, 0.1f + 0.8f * uni(rng), 0.1f + 0.8f * uni(rng), n);
            if (!s.ok) {
                continue;
            }
            const float t0 = s.ray.sigma_enter;
            const float t1 = s.ray.sigma_exit;
            if (!(t1 - t0 > 8.0f)) {
                continue;
            }
            for (int lane = 0; lane < d.lanes; ++lane) {
                const float begin = t0 + (t1 - t0) * (static_cast<float>(lane) /
                                                      static_cast<float>(d.lanes));
                const float end = t0 + (t1 - t0) * (static_cast<float>(lane + 1) /
                                                    static_cast<float>(d.lanes));
                // 段太短（< 2 px）时整段落在起点格里，会被 skip_first_cell 吃掉 ——
                // 那是"段长不足"而不是"步数不足"，不参与本断言。
                if (!(end > begin + 2.0f)) {
                    continue;
                }
                // 只把"段尾最后一格"设成该处射线的视深，其余全是天空
                // ⇒ 命中只可能来自那一格；扫不到就说明步数上限不足。
                // 段尾可能正好压在格边界上（floor 落前一格/后一格都合法）⇒ 取两格，
                // 只要扫到其中一格就算这个上限够用。
                const Vec2 tail_a = PosAt(s.ray, end - 1.0e-3f);
                const Vec2 tail_b = PosAt(s.ray, end - 0.5f);
                const int ax = static_cast<int>(std::floor(tail_a.x));
                const int ay = static_cast<int>(std::floor(tail_a.y));
                const int bx = static_cast<int>(std::floor(tail_b.x));
                const int by = static_cast<int>(std::floor(tail_b.y));
                // 屏幕边缘的格子会被 DDA 的越界 break 吃掉（不是步数问题），不参与断言。
                auto border = [&](int x, int y) {
                    return x <= 0 || y <= 0 || x >= cam.width - 1 || y >= cam.height - 1;
                };
                if (border(ax, ay) || border(bx, by)) {
                    continue;
                }
                auto depth_at = [&](int x, int y) -> float {
                    if (x == ax && y == ay) {
                        return WAt(s.ray, end - 1.0e-3f);
                    }
                    if (x == bx && y == by) {
                        return WAt(s.ray, end - 0.5f);
                    }
                    return 0.0f;
                };
                const float hit = MarchSegment(s.ray, begin, end, d.steps_per_lane,
                                               begin <= 0.0f, depth_at, cam.width, cam.height,
                                               nullptr, nullptr);
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
        const DerivedParams d = DeriveParams(4, cam.width, cam.height);
        for (int trial = 0; trial < 200; ++trial) {
            std::mt19937 rng(7u + static_cast<unsigned>(trial));
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            const Vec3 n = ReconstructNormal(0.2f * (uni(rng) - 0.5f), 0.6f + 0.4f * uni(rng));
            const RaySetup s = MakeRaySetup(cam, 0.2f + 0.6f * uni(rng), 0.2f + 0.6f * uni(rng), n);
            if (!s.ok || !(s.ray.sigma_exit - s.ray.sigma_enter > 8.0f)) {
                continue;
            }
            const float begin = s.ray.sigma_enter;
            const float end = s.ray.sigma_enter +
                              0.25f * (s.ray.sigma_exit - s.ray.sigma_enter);
            const float tail_sigma = end - 1.0e-3f;
            const Vec2 tail = PosAt(s.ray, tail_sigma);
            const int tx = static_cast<int>(std::floor(tail.x));
            const int ty = static_cast<int>(std::floor(tail.y));
            auto depth_at = [&](int x, int y) -> float {
                return (x == tx && y == ty) ? WAt(s.ray, tail_sigma) : 0.0f;
            };
            if (MarchSegment(s.ray, begin, end, d.steps_per_lane, begin <= 0.0f, depth_at,
                             cam.width, cam.height, nullptr, nullptr) < kNoHit) {
                ++full;
            }
            if (MarchSegment(s.ray, begin, end, 1, begin <= 0.0f, depth_at, cam.width,
                             cam.height, nullptr, nullptr) < kNoHit) {
                ++short_hits;
            }
        }
        EXPECT_GT(full, 50) << "满步数下段尾几乎从不命中 ⇒ 上面的断言没覆盖真实情形";
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

    // 回调返回 **w = 1/z**（2026-11-02 起）。
    auto field_flat = [&](int x, int y) -> float {
        (void)x;
        (void)y;
        return 1.0f / 20.0f;
    };
    auto field_two_layers = [&](int x, int y) -> float {
        (void)x;
        return (y >= cam.height / 2) ? (1.0f / 600.0f) : (1.0f / 20.0f);
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
            const DerivedParams d = DeriveParams(4, cam.width, cam.height);
            int hx = -1;
            int hy = -1;
            const float hit =
                (variant == 0)
                    ? MarchSlabs(s.ray, d, field_flat, cam.width, cam.height, &hx, &hy)
                    : MarchSlabs(s.ray, d, field_two_layers, cam.width, cam.height, &hx,
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
    EXPECT_GT(checked, 150) << "样本里命中太少，测不到反推";
    EXPECT_GT(late_hits, 10) << "几乎全是紧贴着起点格的命中 ⇒ 长路径的记账没被覆盖到";
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
    // 返回 w = 1/z；没有墙的列返回 0（= 空）。
    auto depth_at = [&](int x, int y) -> float {
        (void)y;
        float z = 0.0f;
        for (const std::pair<int, float>& c : columns) {
            if (x >= c.first && x < c.first + 17) {
                z = c.second;
            }
        }
        return (z > 0.0f) ? (1.0f / z) : 0.0f;
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
        const float s_ref = MarchWholeRay(s.ray, 100000, depth_at, cam.width,
                                          cam.height, &ref_x, &ref_y);
        for (int tile : {1, 2, 4, 8}) {
            const DerivedParams d = DeriveParams(tile, cam.width, cam.height);
            int sx = -1;
            int sy = -1;
            const float hit =
                MarchSlabs(s.ray, d, depth_at, cam.width, cam.height, &sx, &sy);
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

    // 返回 w = 1/z：x=20/60 处是 10 m 的柱子，其余是 1000 m 的远墙（有几何，w≠0）。
    auto depth_at = [](int x, int y) -> float {
        (void)y;
        if (x == 20 || x == 60) {
            return 1.0f / 10.0f;
        }
        return 1.0f / 1000.0f;
    };

    int hx = -1;
    int hy = -1;
    const float hit = MarchWholeRay(ray, 1000, depth_at, 320, 180, &hx, &hy);
    ASSERT_LT(hit, kNoHit);
    EXPECT_EQ(hx, 20) << "必须命中更近的那处（min 方向反了就会拿到 x=60）";

    // 分段版同样取近的。
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d = DeriveParams(tile, 320, 180);
        int sx = -1;
        int sy = -1;
        const float s = MarchSlabs(ray, d, depth_at, 320, 180, &sx, &sy);
        ASSERT_LT(s, kNoHit) << "tile=" << tile;
        EXPECT_EQ(sx, 20) << "tile=" << tile << " 取到了较远的命中格（min 被写成了 max）";
    }

    // 负向：取 max 会拿到 x=60 —— 显式算一遍，证明上面的断言有牙。
    float latest = -1.0f;
    int lx = -1;
    {
        const DerivedParams d = DeriveParams(4, 320, 180);
        for (int lane = 0; lane < d.lanes; ++lane) {
            const float begin = ray.sigma_enter + (ray.sigma_exit - ray.sigma_enter) *
                                                      (static_cast<float>(lane) /
                                                       static_cast<float>(d.lanes));
            const float end = ray.sigma_enter + (ray.sigma_exit - ray.sigma_enter) *
                                                    (static_cast<float>(lane + 1) /
                                                     static_cast<float>(d.lanes));
            int cx = -1;
            int cy = -1;
            const float s = MarchSegment(ray, begin, end, d.steps_per_lane, begin <= 0.0f,
                                         depth_at, 320, 180, &cx, &cy);
            if (s < kNoHit && s > latest) {
                latest = s;
                lx = cx;
            }
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
    return ray;
}

TEST(SsrMath, BackfaceHitRejected) {
    const ScreenRay ray = MakeSyntheticRay();
    // 2026-11-11 起判据只剩"穿前表面"，所以这两段都**必然**被判成穿越 ——
    // "能不能命中"完全取决于朝向判据（这正是本测试要钉的那条屏幕空间固有限制）。
    // 射线视深沿 σ 从 20 m 线性变到 24 m（w = 1/z 对 σ 仿射）：
    // 注意 s0.y = 20 ⇒ 屏幕行 y 对应 σ = y − 20：
    //   rows 40..59（σ=20..39）⇒ 射线 z ≈ 20.7..21.4 m ⇒ 桌子顶面放 21.0 m
    //   rows 80..99（σ=60..79）⇒ 射线 z ≈ 22.2..23.1 m ⇒ 墙放 22.6 m
    auto depth_at = [](int /*x*/, int y) -> float {
        if (y >= 40 && y < 60) {
            return 1.0f / 21.0f;   // 桌子顶面
        }
        if (y >= 80 && y < 100) {
            return 1.0f / 22.6f;   // 墙
        }
        return 0.0f;               // 空
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
    const float s_old = MarchWholeRay(ray, 200, depth_at, 320, 180,
                                      &old_x, &old_y);
    ASSERT_LT(s_old, kNoHit) << "不判朝向时应该在桌子段命中（否则本测试没造出 bug 场景）";
    EXPECT_GE(old_y, 40);
    EXPECT_LT(old_y, 60);

    // ② 加朝向判据：桌子段被跳过 ⇒ 命中后面的墙。
    int new_x = -1;
    int new_y = -1;
    const float s_new = MarchWholeRay(ray, 200, depth_at, 320, 180,
                                      &new_x, &new_y, facing);
    ASSERT_LT(s_new, kNoHit) << "墙是正面命中，不应被一起拒掉";
    EXPECT_GE(new_y, 80) << "背面命中（桌子顶面）没被跳过";
    EXPECT_GT(s_new, s_old) << "新命中必须比旧命中更远（跳到桌子后面去了）";

    // ③ 防恒真：把墙的法线翻到背面 ⇒ 整条射线应该无命中。
    auto facing_flipped = [](int /*x*/, int /*y*/, const Vec3& d) -> bool {
        return HitFacesRay(d, Vec3{0.0f, 0.0f, -1.0f});
    };
    EXPECT_GE(MarchWholeRay(ray, 200, depth_at, 320, 180, nullptr,
                            nullptr, facing_flipped),
              kNoHit)
        << "法线翻过来仍能命中 ⇒ 朝向判据是恒真的";
}

TEST(SsrMath, DerivedParams) {
    const Cam cam;   // 320×180

    // lanes = tile²；steps 按**屏幕对角线**算（最坏跨度），DDA 到段尾就 break。
    const float diag = std::sqrt(320.0f * 320.0f + 180.0f * 180.0f);
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d = DeriveParams(tile, cam.width, cam.height);
        EXPECT_EQ(d.lanes, tile * tile);
        const float lanes_f = static_cast<float>(d.lanes);
        EXPECT_EQ(d.steps_per_lane,
                  static_cast<int>(std::ceil(1.41421356f * diag / lanes_f)) + 3)
            << "tile=" << tile;
    }

    // 步数上限随 lane 数下降（分得越细，每段越短）。
    const DerivedParams one = DeriveParams(1, cam.width, cam.height);
    const DerivedParams sixteen = DeriveParams(4, cam.width, cam.height);
    EXPECT_GT(one.steps_per_lane, sixteen.steps_per_lane);

    // 分辨率越大，上限越大（同一 tile 下）。
    const DerivedParams big = DeriveParams(4, 1280, 720);
    const DerivedParams small = DeriveParams(4, 320, 180);
    EXPECT_GT(big.steps_per_lane, small.steps_per_lane);
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


// ───────── 升采样的深度值域判据（2026-10-31 掠射横向条纹的根因/回归）─────────
//
// 背景：低分 A 升到全分辨率时要拒绝"跨表面"的邻格。旧判据 =「本像素 w 与低分格子的 w
// 比固定百分比 5%」。**掠射平面上这条必然误判**：地平线附近同一平面相邻两行的深度比
// 就有 1.2 倍以上 ⇒ 每 scale 行里只有"贡献了最小深度"的那一行活着
// ⇒ 实拍得到 period = 降采样倍率的横向条纹（1:2 → 2、1:4 → 4、1:1 无）。

// 修法：1/z 在任何平面上对屏幕坐标**仿射** ⇒ 用「本像素 w + 屏幕梯度·偏移」预测格子
// 应有的 w（且要取格内**最大** w，因为降采样取 min 深度），再比残差。
TEST(SsrMath, UpsampleGrazingPlaneAcceptedAtEveryRow) {
    // 掠射平面：w 对屏幕行仿射（射影几何恒等式）。相邻行相对变化 ≈24% ≫ 5% 容差。
    const float w0 = 0.1389f;   // 行 0 的 1/z
    const float b = 0.0335f;    // 每像素 Δw（向下行更近 ⇒ w 变大）
    auto w_of = [&](int row) { return w0 + b * static_cast<float>(row); };

    for (int scale : {2, 4}) {
        for (int r0 = 0; r0 < 8; ++r0) {
            // 低分格子的值 = 格内**最近**（min 深度 = max w）
            float w_lo = 0.0f;
            for (int i = 0; i < scale; ++i) {
                w_lo = std::max(w_lo, w_of(r0 + i));
            }
            for (int r = r0; r < r0 + scale; ++r) {
                const float w_hi = w_of(r);
                // 本像素中心 → 低分 texel 中心的偏移（单位：全分辨率像素）
                const float dp = (static_cast<float>(r0) + 0.5f * static_cast<float>(scale)) -
                                 (static_cast<float>(r) + 0.5f);
                EXPECT_TRUE(UpsampleDepthAccept(w_hi, /*gw_x=*/0.0f, /*gw_y=*/b, /*dpx=*/0.0f,
                                                dp, w_lo, scale, 0.05f))
                    << "掠射平面 scale=" << scale << " r0=" << r0 << " r=" << r << " 被误拒";
                // 负向对照：**除了贡献最小深度的那一行**，旧判据（不比梯度）在这里必须拒 ——
                // 这正是「每 scale 行只活一行」的来源。最近那一行 w_lo == w_hi，旧判据本来就过，
                // 所以只对 w_hi < w_lo 的行断言。
                if (w_hi < w_lo) {
                    EXPECT_GT(std::fabs(w_lo - w_hi) / w_hi, 0.05f)
                        << "旧判据在 scale=" << scale << " r=" << r << " 竟然通过 ⇒ 场景不够掠射";
                }
            }
        }
    }
}

TEST(SsrMath, UpsampleSilhouetteRejected) {
    // 低分格子取到了**另一个表面**（w 大 250 倍）⇒ 必须拒。这正是深度值域项存在的理由，
    // 修法不能把它一起废掉。
    EXPECT_FALSE(UpsampleDepthAccept(0.002f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 2, 0.05f));
    // 同表面、无梯度 ⇒ 过。
    EXPECT_TRUE(UpsampleDepthAccept(0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 2, 0.05f));
}

TEST(SsrMath, UpsampleScaleOneIsIdenticalToOldRule) {
    // 1:1 时 dpx=dpy=0 且扩展量 (scale−1)=0 ⇒ 判据与旧规则**逐位等价**（1:1 不许有行为变化）。
    const float w_hi = 0.2f;
    for (float w_lo : {0.05f, 0.19f, 0.2f, 0.21f, 0.5f}) {
        const bool old_ok = std::fabs(w_hi - w_lo) / w_hi <= 0.05f;
        const bool new_ok = UpsampleDepthAccept(w_hi, 0.0f, 0.0f, 0.0f, 0.0f, w_lo, 1, 0.05f);
        EXPECT_EQ(old_ok, new_ok) << "w_lo=" << w_lo;
    }
}

}  // namespace

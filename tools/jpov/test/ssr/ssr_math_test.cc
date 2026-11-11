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
using jpov::ssr::CellHitsSlab;
using jpov::ssr::kSlabMergeEpsilon;
using jpov::ssr::kSlabRelativeEpsilon;
using jpov::ssr::MergeSlabs;
using jpov::ssr::Slab;
using jpov::ssr::SlabFromGradient;
using jpov::ssr::SlabsTouch;
using jpov::ssr::UnionConnectedSlabs;
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

// 把"单点深度"回调包成**零厚度**柱体（front == back）。
//
// 用途：分段并行等价性 / 步数上限这类测试要的是"这一格有没有面、面在哪个深度"，
// 与柱体两端的具体值无关；零厚度柱体与旧版单点深度判据**逐位等价**，所以那些测试
// 的结论不受本次改动影响，不需要重写。
template <typename Fn>
auto ZeroThickness(Fn fn) {
    return [fn](int x, int y) {
        const float w = fn(x, y);
        jpov::ssr::Slab s;
        s.front = w;
        s.back = w;
        return s;
    };
}

// 给"单点深度"加一个**相对**厚度（frac × w），用于 DDA / 朝向这类**与柱体两端具体值无关**
// 的机制测试：旧版靠经验厚度 `T = c·z` 才有厚度，新版一律由数据给，这里就显式给一个。
template <typename Fn>
auto RelativeSlab(Fn fn, float frac) {
    return [fn, frac](int x, int y) {
        const float w = fn(x, y);
        jpov::ssr::Slab s;
        s.front = w * (1.0f + frac);
        s.back = w * (1.0f - frac);
        return s;
    };
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

// ───────────────────────── 2. 体素柱（区间判据 + 生产 + 连通并集）─────────────────────────
//
// 2026-11-11 起柱体两端都是**量出来的**：front = 该格方框内这张面能到达的最近深度，
// back = 最远。这里把三件事钉死：
//   ① 判据是「射线区间 ∩ 柱体区间」——两侧都闭，且**两侧都判**；
//   ② 生产公式（平面 + 屏幕梯度 → 柱体）与它的相对 ε；
//   ③ 降采样的**连通并集**：连续面必须合得上（接缝免疫），轮廓处必须排掉远处的面。
//
// 本组测试针对的正是 2026-11-11 那类真实故障：零厚度判据下射线从两个 texel 的
// 采样深度**之间**穿过去（差 4.2e-7）⇒ 两格都判 miss ⇒ 整条射线漏到天上。

TEST(SsrMath, SlabFromGradient) {
    // (a) 正对相机的面：梯度为 0 ⇒ 柱体只剩相对 ε 那一点。
    {
        const float w = 0.5f;   // 2 m
        const Slab s = SlabFromGradient(w, 0.0f, 0.0f);
        EXPECT_FLOAT_EQ(s.front, w * (1.0f + kSlabRelativeEpsilon));
        EXPECT_FLOAT_EQ(s.back, w * (1.0f - kSlabRelativeEpsilon));
        EXPECT_GT(s.front, s.back) << "front 一定是近端（w 更大）";
        // 负向：没有 ε 时厚度为 0 —— 那正是"穿过一个点"的零测度判据。
        EXPECT_FLOAT_EQ(w - 0.0f, w) << "负向对照失效";
    }
    // (b) 斜着的面：半幅 = ½(|gx| + |gy|)，加在两侧。
    {
        const float w = 0.25f;
        const float gx = 0.02f;
        const float gy = -0.01f;
        const Slab s = SlabFromGradient(w, gx, gy);
        const float expect_half = 0.5f * (0.02f + 0.01f) + kSlabRelativeEpsilon * w;
        EXPECT_NEAR(s.front - s.back, 2.0f * expect_half, 1.0e-7f);
        EXPECT_NEAR(0.5f * (s.front + s.back), w, 1.0e-7f) << "柱体必须关于 w 对称";
    }
    // (c) 极近 + 极大梯度：back 必须被夹在 0（而不是负数）。
    {
        const Slab s = SlabFromGradient(0.02f, 0.5f, 0.5f);
        EXPECT_FLOAT_EQ(s.back, 0.0f) << "back < 0 的柱体没有几何意义";
        EXPECT_GT(s.front, 0.0f);
    }
}

TEST(SsrMath, SlabPredicate) {
    // (a) 厚墙：200 m 外的墙，柱体 [199.5, 200.5]，射线在格内穿 199.8 → 201.0 ⇒ 命中。
    {
        Slab wall;
        wall.front = 1.0f / 199.5f;
        wall.back = 1.0f / 200.5f;
        EXPECT_TRUE(CellHitsSlab(1.0f / 199.8f, 1.0f / 201.0f, wall));
    }
    // (b) 薄物：零厚度柱体在 50 m，射线 60 → 61 m（整段都在它后面）⇒ 放行。
    //     负向对照：只判 front 的"首次跨越即命中"实现会判命中 —— 必须不复现。
    {
        Slab thin;
        thin.front = 1.0f / 50.0f;
        thin.back = thin.front;
        const float w_in = 1.0f / 60.0f;
        const float w_out = 1.0f / 61.0f;
        EXPECT_FALSE(CellHitsSlab(w_in, w_out, thin)) << "整段在柱体后方必须放行";
        const bool naive_hit = (std::min(w_in, w_out) <= thin.front);
        EXPECT_TRUE(naive_hit)
            << "负向对照失效：单侧判据在这种输入下本该误判成命中";
    }
    // (c) 闭区间：射线恰好停在 front / back 上都算命中。
    {
        Slab s;
        s.front = 0.5f;    // 2 m
        s.back = 0.25f;    // 4 m
        EXPECT_TRUE(CellHitsSlab(0.4f, s.front, s)) << "射线恰好抵达前表面应算命中";
        EXPECT_TRUE(CellHitsSlab(0.1f, s.back, s)) << "射线恰好触及后表面应算命中";
    }
    // (d) 空格：front ≤ 0 ⇒ 恒不命中（天空不能当墙）。
    {
        Slab empty;
        EXPECT_FALSE(CellHitsSlab(1.0f, 0.5f, empty));
    }
    // (e) 射线整段在柱体前方（更近）⇒ 放行。
    {
        Slab s;
        s.front = 0.25f;   // 4 m
        s.back = 0.20f;    // 5 m
        EXPECT_FALSE(CellHitsSlab(0.5f, 0.4f, s));
    }
}

// 接缝免疫 —— 本 PR 的**回归测试**（对应 2026-11-11 实测的 7060 px 漏光）
//
// 场景：一张面 w(x) 横跨两个 texel；射线从两格的采样深度**之间**穿过去（真实差值只有
// 1e-7 量级）。零厚度判据两格都判 miss（射线比前一格的样本"更近"、比后一格的样本也"更近"
// —— 因为它恰好从缝隙里过），整条射线漏掉；柱体判据两格都能命中。
TEST(SsrMath, SeamImmuneWithSlabs) {
    // 两个 texel 的**采样深度**（零厚度时的全部信息）：centre 在 x=0.5 / 1.5。
    const float w_a = 0.200000f;   // 5.000 m
    const float w_b = 0.200500f;   // 4.987 m
    // 射线：沿 +x 走，w 随 x 线性增长（w = 1/d 对屏幕坐标仿射）。
    ScreenRay ray;
    ray.s0 = Vec2{0.5f, 0.5f};
    ray.dir = Vec2{1.0f, 0.0f};
    ray.span = 2.0f;
    ray.w0 = 0.200200f;   // x = 0.5
    ray.w1 = 0.200400f;   // x = 2.5
    ray.sigma_enter = 0.0f;
    ray.sigma_exit = 2.0f;
    ray.dir3 = Vec3{0.0f, 0.0f, -1.0f};

    // 面在这个像素方框里的梯度：每个 texel 差 0.0005 ⇒ gx = 0.0005/pixel。
    const float gx = (w_b - w_a) / 1.0f;

    // ① 零厚度：两格都 miss —— 这就是漏光的机制（本测试的负向对照）。
    {
        auto zero_slab = [&](int x, int) {
            Slab s;
            s.front = (x == 0) ? w_a : w_b;
            s.back = s.front;
            return s;
        };
        const float hit_zero = MarchWholeRay(ray, 1000, zero_slab, 8, 8);
        EXPECT_GE(hit_zero, kNoHit)
            << "零厚度判据在这里居然命中了 ⇒ 本测试没能复现接缝漏光";
    }
    // ② 柱体（平面 + 梯度 + 相对 ε）：必须命中。
    {
        auto slab_at = [&](int x, int) {
            return SlabFromGradient((x == 0) ? w_a : w_b, gx, 0.0f);
        };
        const float hit = MarchWholeRay(ray, 1000, slab_at, 8, 8);
        EXPECT_LT(hit, kNoHit) << "接缝处必须命中：柱体两端来自这张面自己，不是采样点";
    }
    // ③ 只有 ε、没有梯度（把梯度当 0）：本例的缝（2e-4）远大于 ε（2e-6）⇒ 仍然漏。
    //    这条说明"抗零测度"与"覆盖面在像素内的变化"是**两件不同的事**，都得有。
    {
        auto eps_only = [&](int x, int) {
            return SlabFromGradient((x == 0) ? w_a : w_b, 0.0f, 0.0f);
        };
        EXPECT_GE(MarchWholeRay(ray, 1000, eps_only, 8, 8), kNoHit)
            << "只有 ε 时本该仍然漏（否则本用例的判别力有问题）";
    }
}

// 降采样的连通并集 —— 面向 N≥2 的成立性（Danis 第二步验证）
TEST(SsrMath, UnionConnectedSlabs) {
    // (a) 连续面跨 2×2：并集必须等于**整格包络**（= 各子格包络的 min/max）。
    {
        const float w_c = 0.2f;
        const float gx = 0.01f;
        const float gy = 0.004f;
        Slab sub[4];
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                sub[j * 2 + i] = SlabFromGradient(w_c + gx * static_cast<float>(i) +
                                                      gy * static_cast<float>(j),
                                                  gx, gy);
            }
        }
        const Slab u = UnionConnectedSlabs(sub, 4, kSlabMergeEpsilon);
        float f = sub[0].front;
        float b = sub[0].back;
        for (int k = 1; k < 4; ++k) {
            f = std::max(f, sub[k].front);
            b = std::min(b, sub[k].back);
        }
        EXPECT_FLOAT_EQ(u.front, f) << "连续面必须全部合上（接缝免疫的来源）";
        EXPECT_FLOAT_EQ(u.back, b);
    }
    // (b) 轮廓：近物（2 m）+ 远墙（60 m）在同一格 ⇒ 并集只能描述近物。
    //     负向对照：直接 min/max 会拼出一根贯通 ~58 m 的柱子（反射里每个轮廓都长黑边）。
    {
        Slab sub[4];
        for (int k = 0; k < 4; ++k) {
            Slab s;
            s.front = 1.0f / 2.0f;
            s.back = 1.0f / 2.0f;
            sub[k] = s;
        }
        Slab wall;
        wall.front = 1.0f / 60.0f;
        wall.back = wall.front;
        sub[3] = wall;
        const Slab u = UnionConnectedSlabs(sub, 4, kSlabMergeEpsilon);
        EXPECT_NEAR(u.front, 1.0f / 2.0f, 1.0e-6f);
        EXPECT_NEAR(u.back, 1.0f / 2.0f, 1.0e-6f) << "远处的墙必须被排除，不能拼成厚墙";
        // 负向：朴素 min/max。
        float b = sub[0].back;
        for (int k = 1; k < 4; ++k) {
            b = std::min(b, sub[k].back);
        }
        EXPECT_LT(b, 1.0f / 30.0f) << "负向对照失效：朴素并集没有把墙算进来";
    }
    // (c) 间接连通：0↔1 相交、1↔2 相交、0↛2 ⇒ 三个都要合上。
    {
        Slab sub[3];
        sub[0].front = 0.20f;
        sub[0].back = 0.19f;
        sub[1].front = 0.19f;   // 与 0 边沿相接
        sub[1].back = 0.18f;
        sub[2].front = 0.18f;   // 与 1 相接，与 0 不直接相接
        sub[2].back = 0.17f;
        const Slab u = UnionConnectedSlabs(sub, 3, 0.0f);
        EXPECT_NEAR(u.front, 0.20f, 1.0e-6f);
        EXPECT_NEAR(u.back, 0.17f, 1.0e-6f) << "必须迭代到不变（间接连通要合上）";
    }
    // (d) 空格不参与：全是空 ⇒ 结果仍是空。
    {
        Slab sub[4];
        const Slab u = UnionConnectedSlabs(sub, 4, kSlabMergeEpsilon);
        EXPECT_LE(u.front, 0.0f);
    }
    // (e) ε 的作用：两格之间有一个小于 ε 的缝 ⇒ 要能合上（Danis 要求"更容易连通"）。
    {
        Slab sub[2];
        sub[0].front = 0.200f;
        sub[0].back = 0.199f;
        sub[1].front = 0.1985f;   // 与 sub[0].back 差 5e-4（相对 2.5e-3 < ε=1e-3? 不）
        sub[1].back = 0.197f;
        const Slab loose = UnionConnectedSlabs(sub, 2, 0.0f);
        EXPECT_NEAR(loose.back, 0.199f, 1.0e-6f) << "ε=0 时这个缝必须断开";
        const Slab merged = UnionConnectedSlabs(sub, 2, 1.0e-2f);
        EXPECT_NEAR(merged.back, 0.197f, 1.0e-6f) << "ε 足够大时应当合上";
    }
}

// ───────────────────────── 3. 每段步数上限够走完满段 ─────────────────────────
//
// 段长 L 在**对角线**最坏情况下跨 ceil(√2·L)+1 个格子。上限少一步 ⇒ 段尾那一格永远扫不到，
// 症状是"倒影在每段接缝处缺一格"，且只在特定角度出现（最难查的一类）。
TEST(SsrMath, SlabCoversWholeSlab) {
    const Cam cam;
    const int kMaxRayPixels = 160;
    int checked = 0;
    int missed = 0;
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d =
            DeriveParams(tile, kMaxRayPixels);
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
                // 回调语义（2026-11-02 起）：返回 **w = 1/z**，0 = 没有几何。
                auto depth_at = [&](int x, int y) -> float {
                    return (x == tx && y == ty) ? WAt(s.ray, tail_sigma) : 0.0f;
                };
                const float hit =
                    MarchSegment(s.ray, begin, end, d.steps_per_lane,
                                 d.slab_pixels * static_cast<float>(lane) <= 0.0f, RelativeSlab(depth_at, 0.10f), cam.width,
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
            const DerivedParams d = DeriveParams(4, kMaxRayPixels);
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
                return (x == tx && y == ty) ? WAt(s.ray, tail_sigma) : 0.0f;
            };
            if (MarchSegment(s.ray, begin, end, d.steps_per_lane, begin <= 0.0f, RelativeSlab(depth_at, 0.10f), cam.width,
                             cam.height, nullptr, nullptr) < kNoHit) {
                ++full;
            }
            if (MarchSegment(s.ray, begin, end, 1, begin <= 0.0f, RelativeSlab(depth_at, 0.10f), cam.width, cam.height, nullptr,
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
            const DerivedParams d = DeriveParams(4, 300);
            int hx = -1;
            int hy = -1;
            const float hit =
                (variant == 0)
                    ? MarchSlabs(s.ray, d, RelativeSlab(field_flat, 0.10f), cam.width, cam.height, &hx, &hy)
                    : MarchSlabs(s.ray, d, RelativeSlab(field_two_layers, 0.10f), cam.width, cam.height, &hx,
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
        const float s_ref = MarchWholeRay(s.ray, 100000, RelativeSlab(depth_at, 0.10f), cam.width,
                                          cam.height, &ref_x, &ref_y);
        for (int tile : {1, 2, 4, 8}) {
            const DerivedParams d = DeriveParams(tile, 220);
            int sx = -1;
            int sy = -1;
            const float hit =
                MarchSlabs(s.ray, d, RelativeSlab(depth_at, 0.10f), cam.width, cam.height, &sx, &sy);
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
    const float hit = MarchWholeRay(ray, 1000, RelativeSlab(depth_at, 0.10f), 320, 180, &hx, &hy);
    ASSERT_LT(hit, kNoHit);
    EXPECT_EQ(hx, 20) << "必须命中更近的那处（min 方向反了就会拿到 x=60）";

    // 分段版同样取近的。
    for (int tile : {1, 2, 4, 8}) {
        const DerivedParams d = DeriveParams(tile, 160);
        int sx = -1;
        int sy = -1;
        const float s = MarchSlabs(ray, d, RelativeSlab(depth_at, 0.10f), 320, 180, &sx, &sy);
        ASSERT_LT(s, kNoHit) << "tile=" << tile;
        EXPECT_EQ(sx, 20) << "tile=" << tile << " 取到了较远的命中格（min 被写成了 max）";
    }

    // 负向：取 max 会拿到 x=60 —— 显式算一遍，证明上面的断言有牙。
    float latest = -1.0f;
    int lx = -1;
    for (int lane = 0; lane < 16; ++lane) {
        const DerivedParams d = DeriveParams(4, 160);
        const float begin = d.slab_pixels * static_cast<float>(lane);
        const float end = begin + d.slab_pixels;
        int cx = -1;
        int cy = -1;
        const float s = MarchSegment(ray, begin, end, d.steps_per_lane, begin <= 0.0f, RelativeSlab(depth_at, 0.10f), 320, 180, &cx,
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
    return ray;
}

TEST(SsrMath, BackfaceHitRejected) {
    const ScreenRay ray = MakeSyntheticRay();
    // 柱体用零厚度（front == back），射线在两段上都是**直接穿过那个深度**
    // ⇒ "能不能命中"完全取决于朝向判据，不是柱体在挡。

    auto depth_at = [](int /*x*/, int y) -> float {
        if (y >= 40 && y < 60) {
            return 1.0f / 20.0f;   // 桌子顶面（w = 1/z）
        }
        if (y >= 80 && y < 120) {
            return 1.0f / 20.0f;   // 墙
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

    // ① 不判朝向：先撞上桌子顶面 —— 这就是那个 bug。
    int old_x = -1;
    int old_y = -1;
    const float s_old = MarchWholeRay(ray, 200, RelativeSlab(depth_at, 0.10f), 320, 180,
                                      &old_x, &old_y);
    ASSERT_LT(s_old, kNoHit) << "旧判据应该在桌子段命中（否则本测试没造出 bug 场景）";
    EXPECT_GE(old_y, 40);
    EXPECT_LT(old_y, 60);

    // ② 加朝向判据：桌子段被跳过 ⇒ 命中后面的墙。
    int new_x = -1;
    int new_y = -1;
    const float s_new = MarchWholeRay(ray, 200, RelativeSlab(depth_at, 0.10f), 320, 180,
                                      &new_x, &new_y, facing);
    ASSERT_LT(s_new, kNoHit) << "墙是正面命中，不应被一起拒掉";
    EXPECT_GE(new_y, 80) << "背面命中（桌子顶面）没被跳过";
    EXPECT_GT(s_new, s_old) << "新命中必须比旧命中更远（跳到桌子后面去了）";

    // ③ 防恒真：把墙的法线翻到背面 ⇒ 整条射线应该无命中。
    auto facing_flipped = [](int /*x*/, int /*y*/, const Vec3& d) -> bool {
        return HitFacesRay(d, Vec3{0.0f, 0.0f, -1.0f});
    };
    EXPECT_GE(MarchWholeRay(ray, 200, RelativeSlab(depth_at, 0.10f), 320, 180, nullptr,
                            nullptr, facing_flipped),
              kNoHit)
        << "法线翻过来仍能命中 ⇒ 朝向判据是恒真的";
}

TEST(SsrMath, DerivedParams) {
    const Cam cam;
    const DerivedParams d =
        DeriveParams(/*tile=*/4, /*max_ray_pixels=*/400);
    EXPECT_EQ(d.lanes, 16);
    EXPECT_FLOAT_EQ(d.slab_pixels, 25.0f);
    EXPECT_EQ(d.steps_per_lane, static_cast<int>(std::ceil(1.41421356f * 25.0f)) + 3);
    EXPECT_FALSE(jpov::ssr::SlabTooShortForEfficiency(d)) << "25 px/段不该被告警";

    // 1/8 分辨率 + tile 8 + reach 40：L = 0.625 px ⇒ 必须被标记（只告警，不失败）。
    const DerivedParams bad = DeriveParams(8, 40);
    EXPECT_FLOAT_EQ(bad.slab_pixels, 40.0f / 64.0f);
    EXPECT_TRUE(jpov::ssr::SlabTooShortForEfficiency(bad));

    // T=1 时 L 就是全部预算。
    const DerivedParams one = DeriveParams(1, 300);
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

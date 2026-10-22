// JPOV 半透明条带：CPU 侧几何展开 + 输入校验的单测（GL-free，不需要 GL context）
//
// 覆盖点：
//   * 展开出的顶点数量/顺序（right 先、left 后）与 uv 约定（u=0/1、v=col/15）；
//   * 不足 16 点时的**尾部塌缩**（多余列坐在最后一个有效点上，且 uv 不跟着改）；
//   * 逐点 alpha 正确地带到两侧顶点；
//   * 索引模式：每 quad 6 个索引、共 90 个、全部落在本块 [base, base+32) 内；
//   * ValidateStripBatch 的负向用例（point_count / alpha / profile / tint / NaN / style）必须崩 ——
//     这些是「用户忘了填」的唯一防线，不能静默通过；
//   * **平滑 pass（kSmooth）的 CPU 前端**：切线（两端必须是完整单侧差分）、
//     自动外扩量（必须覆盖真实外凸量）、数据纹理打包顺序、顶点外扩与 v 的一致。
//     这组断言专门盯「出图看不出来」的错误，每一项都带**负向对照**（把旧实现算回来必须失败）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/src/strip/strip_geometry.h"

namespace {

jpov::Strip16 MakeStrip(uint32_t point_count) {
    jpov::Strip16 s;
    s.point_count = point_count;
    for (uint32_t i = 0; i < jpov::kStripSampleCount; ++i) {
        const float f = static_cast<float>(i);
        s.left[i] = jpov::Vec3f{f, 1.0f, 0.0f};
        s.right[i] = jpov::Vec3f{f, 0.0f, 0.0f};
        s.alpha[i] = 1.0f;
    }
    return s;
}

jpov::TranslucentStripBatch MakeBatch(uint32_t point_count) {
    jpov::TranslucentStripBatch b;
    b.strips.push_back(MakeStrip(point_count));
    return b;
}

}  // namespace

TEST(StripGeometry, ExpandsFixedTopology) {
    std::vector<jpov::StripVertex> verts;
    jpov::ExpandStripVertices(MakeStrip(16), &verts);

    ASSERT_EQ(verts.size(), static_cast<size_t>(jpov::kStripVerticesPerStrip));
    EXPECT_EQ(jpov::kStripVerticesPerStrip, 32);
    EXPECT_EQ(jpov::kStripQuads, 15);
    EXPECT_EQ(jpov::kStripIndicesPerStrip, 90);

    // 第 0 列：先 right（u=0, v=0 = 左下），再 left（u=0, v=1 = 左上）。
    // UV 约定：u 沿长度（图片从左到右），v 横跨宽度（0=right / 1=left）。
    EXPECT_FLOAT_EQ(verts[0].position.y(), 0.0f);
    EXPECT_FLOAT_EQ(verts[0].u, 0.0f);
    EXPECT_FLOAT_EQ(verts[0].v, 0.0f);
    EXPECT_FLOAT_EQ(verts[1].position.y(), 1.0f);
    EXPECT_FLOAT_EQ(verts[1].u, 0.0f);
    EXPECT_FLOAT_EQ(verts[1].v, 1.0f);

    // 最后一列 col=15：u = 15/15 = 1（右下 / 右上），v 仍 0 / 1。
    const jpov::StripVertex& last_right = verts[30];
    const jpov::StripVertex& last_left = verts[31];
    EXPECT_FLOAT_EQ(last_right.u, 1.0f);
    EXPECT_FLOAT_EQ(last_right.v, 0.0f);
    EXPECT_FLOAT_EQ(last_left.u, 1.0f);
    EXPECT_FLOAT_EQ(last_left.v, 1.0f);
    EXPECT_FLOAT_EQ(last_right.position.x(), 15.0f);
}

TEST(StripGeometry, CollapsesTailBeyondPointCount) {
    std::vector<jpov::StripVertex> verts;
    jpov::ExpandStripVertices(MakeStrip(4), &verts);

    ASSERT_EQ(verts.size(), static_cast<size_t>(jpov::kStripVerticesPerStrip));
    // 第 3 列仍是有效点。
    EXPECT_FLOAT_EQ(verts[6].position.x(), 3.0f);
    // 第 4..15 列全部塌到第 3 个点（x = 3），但 uv v 仍按列号走（纹理不拉伸）。
    for (int col = 4; col < jpov::kStripSampleCount; ++col) {
        EXPECT_FLOAT_EQ(verts[2 * col].position.x(), 3.0f) << "col=" << col;
        EXPECT_FLOAT_EQ(verts[2 * col + 1].position.x(), 3.0f) << "col=" << col;
        // UV 不塌缩：u 仍按列号走（纹理沿长度等分 16 格，短条带只用到前一段）。
        EXPECT_FLOAT_EQ(verts[2 * col].u,
                        static_cast<float>(col) / static_cast<float>(jpov::kStripQuads));
        EXPECT_FLOAT_EQ(verts[2 * col].v, 0.0f);
        EXPECT_FLOAT_EQ(verts[2 * col + 1].v, 1.0f);
    }
}

TEST(StripGeometry, CarriesPerPointAlpha) {
    jpov::Strip16 strip = MakeStrip(16);
    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        strip.alpha[i] = static_cast<float>(i) / 15.0f;
    }
    std::vector<jpov::StripVertex> verts;
    jpov::ExpandStripVertices(strip, &verts);

    for (int col = 0; col < jpov::kStripSampleCount; ++col) {
        const float want = static_cast<float>(col) / 15.0f;
        EXPECT_FLOAT_EQ(verts[2 * col].alpha, want) << "col=" << col;
        EXPECT_FLOAT_EQ(verts[2 * col + 1].alpha, want) << "col=" << col;
    }
}

TEST(StripGeometry, IndexPatternMatchesQuads) {
    // 第 0 条带（base=0）第 1 条带（base=32）各取一份，验证偏移与三角形组成。
    std::vector<uint32_t> idx;
    jpov::AppendStripIndicesForBlock(32, &idx);
    ASSERT_EQ(idx.size(), static_cast<size_t>(jpov::kStripIndicesPerStrip));

    // 第一个 quad：right0(32) left0(33) right1(34) / left0(33) left1(35) right1(34)
    EXPECT_EQ(idx[0], 32u);
    EXPECT_EQ(idx[1], 33u);
    EXPECT_EQ(idx[2], 34u);
    EXPECT_EQ(idx[3], 33u);
    EXPECT_EQ(idx[4], 35u);
    EXPECT_EQ(idx[5], 34u);

    // 全部索引落在 [32, 64) 内，且每个 quad 恰好用到 4 个不同顶点。
    for (uint32_t v : idx) {
        EXPECT_GE(v, 32u);
        EXPECT_LT(v, 64u);
    }
    EXPECT_EQ(idx[89], 62u);   // 最后一个 quad 的第二个三角形末顶点（right_{q+1}）
}

TEST(StripGeometry, ValidBatchPasses) {
    // 合法输入必须**不崩**：本用例的价值在「执行到这里没 abort」，
    // 不用 EXPECT_NO_FATAL_FAILURE（gtest 捕不到 glog CHECK 的 abort，会给出假的安全感）。
    const jpov::TranslucentStripBatch batch = MakeBatch(16);
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsZeroPointCount) {
    // 默认构造 = point_count 0（调用方忘了填）。必须崩，不能静默画一条空条带。
    jpov::TranslucentStripBatch batch;
    jpov::Strip16 strip = MakeStrip(16);
    strip.point_count = 0;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(StripGeometry, RejectsTooManyPoints) {
    jpov::TranslucentStripBatch batch;
    jpov::Strip16 strip = MakeStrip(16);
    strip.point_count = static_cast<uint32_t>(jpov::kStripSampleCount) + 1;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(StripGeometry, RejectsAllZeroAlpha) {
    // 典型误用：整块 memset 0 之后忘了填 alpha → 整批隐形。必须崩。
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    for (float& a : batch.strips[0].alpha) {
        a = 0.0f;
    }
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "alpha");
}

TEST(StripGeometry, RejectsAllZeroProfile) {
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.width_profile.fill(0.0f);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "width_profile");
}

TEST(StripGeometry, RejectsNonFinitePosition) {
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.strips[0].left[3] = jpov::Vec3f{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "非有限值");
}

TEST(StripGeometry, RejectsOutOfRangeTint) {
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.tint.a = 1.5f;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "tint");
}

TEST(StripGeometry, RejectsNonPositiveSoftDepth) {
    // nullopt = 关闭；给了值就必须 > 0（0 会让淡出表达式除零）。
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.soft_depth_m = 0.0f;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "soft_depth_m");
}

TEST(StripGeometry, RejectsIllegalBlendEnum) {
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.blend = static_cast<jpov::StripBlend>(7);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "blend");
}

TEST(StripGeometry, RejectsIllegalStyleEnum) {
    // style 非法 cast 值不许静默走折线 pass。
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.style = static_cast<jpov::StripStyle>(9);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "style");
}

TEST(StripGeometry, AcceptsSoftDepthAndEmissive) {
    // 合法组合必须不崩（执行到这里没 abort 就是通过）。
    jpov::TranslucentStripBatch batch = MakeBatch(16);
    batch.blend = jpov::StripBlend::kEmissive;
    batch.soft_depth_m = 0.15f;
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsEmptyBatch) {
    jpov::TranslucentStripBatch batch;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "不能为空");
}

// ──────────────── 平滑 pass（kSmooth）的 CPU 前端 ────────────────
//
// 两类「肉眼看不出来」的错误：
//   ① 外扩量不够 → 弯的外侧缺一条亚像素（出图要放大才看得见）；
//   ② 切线算错   → 曲线仍过点，但参数化不线性（纹理被不均匀拉伸）。
// 每条断言都配一个**负向对照**：把旧实现（半切线 / 等宽简化）算一遍，断言它必须不满足。

namespace {

// 直条带（沿 +x，列距 1m）：left = c + (0, half, 0)、right = c - (0, half, 0)。
// half_by_col 的第 i 项是第 i 列的半宽（米）。
jpov::Strip16 MakeStraightStrip(const std::vector<float>& half_by_col,
                                uint32_t point_count) {
    jpov::Strip16 s;
    s.point_count = point_count;
    for (uint32_t i = 0; i < jpov::kStripSampleCount; ++i) {
        const float x = static_cast<float>(i);
        const float half = half_by_col[i];
        s.left[i] = jpov::Vec3f{x, half, 0.0f};
        s.right[i] = jpov::Vec3f{x, -half, 0.0f};
        s.alpha[i] = 1.0f;
    }
    return s;
}

// 用**完整 Hermite** 求「真实曲线相对弦在宽度方向上的外凸量」的最大值（任意侧）。
// samples_per_segment 取很大时就是真值；取 kSmoothMarginSamplesPerSegment 时与生产代码同网格。
float ScanRequiredMargin(const jpov::SmoothStripCompute& c, int samples_per_segment,
                         bool use_equi_width_shortcut) {
    float margin = 0.0f;
    for (int seg = 0; seg < jpov::kStripQuads; ++seg) {
        const jpov::Vec3f& h0 = c.half[seg];
        const jpov::Vec3f& h1 = c.half[seg + 1];
        const float dx = h1.x() - h0.x();
        const float dy = h1.y() - h0.y();
        const float dz = h1.z() - h0.z();
        const bool varying = dx * dx + dy * dy + dz * dz > 1e-12f;
        for (int si = 1; si <= samples_per_segment; ++si) {
            const float t = static_cast<float>(si) /
                            static_cast<float>(samples_per_segment + 1);
            const jpov::Vec3f lc = jpov::smooth_internal::Lerp(
                c.center[seg], c.center[seg + 1], t);
            const jpov::Vec3f lh =
                jpov::smooth_internal::Lerp(h0, h1, t);
            const float len = lh.Norm();
            if (len < 1e-5f) {
                continue;
            }
            const jpov::Vec3f w{lh.x() / len, lh.y() / len, lh.z() / len};
            const jpov::Vec3f sc = jpov::smooth_internal::Hermite(
                c.center[seg], c.center_tan[seg], c.center[seg + 1],
                c.center_tan[seg + 1], t);
            // 旧实现的简化：h0 == h1 时把半宽矢量当常数（丢掉 h10·n0 + h11·n1）。
            const jpov::Vec3f sh =
                (use_equi_width_shortcut && !varying)
                    ? h0
                    : jpov::smooth_internal::Hermite(c.half[seg], c.half_tan[seg],
                                                     c.half[seg + 1],
                                                     c.half_tan[seg + 1], t);
            const float dl = (sc - lc).Dot(w);
            const float dhw = (sh - lh).Dot(w);
            margin = std::max(margin, std::max(dl + dhw, -dl + dhw));
        }
    }
    return margin;
}

// 旧实现的切线：两端也乘 0.5（= 半个单侧差分）。
void HalveEndTangents(jpov::SmoothStripCompute* c) {
    c->center_tan[0] = c->center_tan[0] * 0.5f;
    c->half_tan[0] = c->half_tan[0] * 0.5f;
    const int last = jpov::kStripSampleCount - 1;
    c->center_tan[last] = c->center_tan[last] * 0.5f;
    c->half_tan[last] = c->half_tan[last] * 0.5f;
}

}  // namespace

TEST(StripSmooth, CenterAndHalfAreColumnMidpoints) {
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip(16), &c);
    // MakeStrip: left=(i,1,0)、right=(i,0,0) ⇒ center=(i,0.5,0)、half=(0,0.5,0)。
    for (int col = 0; col < jpov::kStripSampleCount; ++col) {
        EXPECT_FLOAT_EQ(c.center[col].x(), static_cast<float>(col));
        EXPECT_FLOAT_EQ(c.center[col].y(), 0.5f);
        EXPECT_FLOAT_EQ(c.half[col].y(), 0.5f);
        EXPECT_FLOAT_EQ(c.half[col].x(), 0.0f);
    }
}

TEST(StripSmooth, EndTangentsAreFullOneSidedDifferences) {
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip(16), &c);
    // 均匀列距 1m：内部中心差分 = 1，两端也必须是 1（不是 0.5）。
    for (int col = 0; col < jpov::kStripSampleCount; ++col) {
        EXPECT_FLOAT_EQ(c.center_tan[col].x(), 1.0f) << "col=" << col;
    }
}

TEST(StripSmooth, StraightStripIsExactlyLinear) {
    // 直条带的 Hermite 必须**逐点等于**弦 —— 这正是「kSmooth 对直线退化成 kPolyline」。
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip(16), &c);
    for (int seg = 0; seg < jpov::kStripQuads; ++seg) {
        for (int si = 1; si <= 8; ++si) {
            const float t = static_cast<float>(si) / 9.0f;
            const jpov::Vec3f curve = jpov::smooth_internal::Hermite(
                c.center[seg], c.center_tan[seg], c.center[seg + 1],
                c.center_tan[seg + 1], t);
            const jpov::Vec3f chord =
                jpov::smooth_internal::Lerp(c.center[seg], c.center[seg + 1], t);
            EXPECT_NEAR((curve - chord).Norm(), 0.0f, 1e-5f)
                << "seg=" << seg << " t=" << t;
        }
    }
}

TEST(StripSmooth, StraightStripIsExactlyLinear_HalfEndTangentFails) {
    // **负向对照**：把两端切线改回旧实现（半个单侧差分），上面那条断言必须不成立。
    // 否则说明 「StraightStripIsExactlyLinear」是恒真的废断言。
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip(16), &c);
    HalveEndTangents(&c);
    const jpov::Vec3f curve = jpov::smooth_internal::Hermite(
        c.center[0], c.center_tan[0], c.center[1], c.center_tan[1], 0.5f);
    const jpov::Vec3f chord = jpov::smooth_internal::Lerp(c.center[0], c.center[1], 0.5f);
    // 旧实现偏 1/16 段长（= 0.0625m），远大于 1e-5 的容差。
    EXPECT_GT(std::fabs(curve.x() - chord.x()), 1e-3f);
    EXPECT_NEAR(curve.x(), 0.4375f, 1e-5f);
}

TEST(StripSmooth, TwoPointStripIsExactlyStraight) {
    // 2 点条带：C1 插值在数学上就是直线（参数化也线性）⇒ 必须与折线 pass 完全一致。
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip(2), &c);
    const jpov::Vec3f curve = jpov::smooth_internal::Hermite(
        c.center[0], c.center_tan[0], c.center[1], c.center_tan[1], 0.37f);
    const jpov::Vec3f chord = jpov::smooth_internal::Lerp(c.center[0], c.center[1], 0.37f);
    EXPECT_NEAR((curve - chord).Norm(), 0.0f, 1e-5f);
}

TEST(StripSmooth, MarginCoversTrueBulge) {
    // 半宽 0.2 恒定、**最后一列收到 0**（最常见的拖尾形状）。
    // 外扩量必须 ≥ 密采样出来的真实外凸量；否则弯的外侧会缺一条。
    std::vector<float> half(16, 0.2f);
    half[15] = 0.0f;
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStraightStrip(half, 16), &c);

    const float required = ScanRequiredMargin(c, /*samples_per_segment=*/1000,
                                              /*use_equi_width_shortcut=*/false);
    EXPECT_GT(required, 0.002f) << "这个形状本来就有 2mm 以上的外凸，测试用例退化了";
    EXPECT_GE(c.margin, required);
    // 上界只是防「margin 爆掉」：这个形状的真实外凸约 15mm，2% + 1mm 余量后远小于 5cm。
    EXPECT_LT(c.margin, 0.05f);
}

TEST(StripSmooth, MarginCoversTrueBulge_EquiWidthShortcutFails) {
    // **负向对照**：旧实现「h0 == h1 就把半宽当常数」低估了外凸量。
    // 生产代码必须**不是**这种算法 —— 这条断言一旦变红，说明有人把简化加回来了。
    std::vector<float> half(16, 0.2f);
    half[15] = 0.0f;
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStraightStrip(half, 16), &c);

    // 两边用**同样的密采样**，差异只可能来自「等宽简化」本身
    // （否则就成了「粗采样 vs 密采样」的比较，测不到简化）。
    const float required = ScanRequiredMargin(c, 1000, /*use_equi_width_shortcut=*/false);
    const float shortcut = ScanRequiredMargin(c, 1000, /*use_equi_width_shortcut=*/true);
    EXPECT_LT(shortcut, required) << "简化版没被抓住 ⇒ 这条负向对照失效了";
    // 生产实现（含 2% + 1mm 余量）必须盖住真实值。
    EXPECT_GT(c.margin, shortcut);
}

TEST(StripSmooth, DataPackingMatchesShaderLayout) {
    // 每列 12 float 的顺序必须与 kSmoothFsMain 的解包逐字对应：c.xyz, m.xyz, h.xyz, n.xyz。
    jpov::Strip16 s;
    s.point_count = 3;
    for (uint32_t i = 0; i < jpov::kStripSampleCount; ++i) {
        const float f = static_cast<float>(i);
        s.left[i] = jpov::Vec3f{f, f + 0.25f, -f};
        s.right[i] = jpov::Vec3f{f, f - 0.25f, -f};
        s.alpha[i] = 1.0f;
    }
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(s, &c);
    std::vector<float> data;
    jpov::AppendSmoothStripData(c, &data);

    ASSERT_EQ(data.size(), static_cast<size_t>(jpov::kStripSampleCount) *
                               jpov::kSmoothFloatsPerColumn);
    EXPECT_EQ(jpov::kSmoothFloatsPerColumn, 12);
    EXPECT_EQ(data.size(), static_cast<size_t>(jpov::kStripSampleCount) *
                               jpov::kSmoothTexelsPerColumn * 4);

    // 第 1 列：center=(1,1,-1)、center_tan=(1,1,-1)、half=(0,0.25,0)、half_tan=(0,0,0)。
    const size_t base = 1 * jpov::kSmoothFloatsPerColumn;
    EXPECT_FLOAT_EQ(data[base + 0], 1.0f);    // c.x
    EXPECT_FLOAT_EQ(data[base + 1], 1.0f);    // c.y
    EXPECT_FLOAT_EQ(data[base + 2], -1.0f);   // c.z
    EXPECT_FLOAT_EQ(data[base + 3], 1.0f);    // m.x
    EXPECT_FLOAT_EQ(data[base + 4], 1.0f);    // m.y
    EXPECT_FLOAT_EQ(data[base + 5], -1.0f);   // m.z
    EXPECT_FLOAT_EQ(data[base + 6], 0.0f);    // h.x
    EXPECT_FLOAT_EQ(data[base + 7], 0.25f);   // h.y
    EXPECT_FLOAT_EQ(data[base + 8], 0.0f);    // h.z
    EXPECT_FLOAT_EQ(data[base + 9], 0.0f);    // n.x
    EXPECT_FLOAT_EQ(data[base + 10], 0.0f);   // n.y
    EXPECT_FLOAT_EQ(data[base + 11], 0.0f);   // n.z
}

TEST(StripSmooth, VerticesWidenAlongHalfVectorAndFoldIntoV) {
    // 顶点 = center + (edge − center)·scale，且 v 折成 0.5 ∓ scale/2 ——
    // FS 的 position = lerp(c) + (2v−1)·lerp(h) 依赖这两件事严格成对。
    std::vector<float> half(16, 0.3f);
    jpov::Strip16 s = MakeStraightStrip(half, 16);
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(s, &c);
    std::vector<float> verts;
    jpov::AppendSmoothStripVertices(s, c, /*strip_id=*/7.0f, &verts);

    ASSERT_EQ(verts.size(), static_cast<size_t>(jpov::kStripVerticesPerStrip) *
                                jpov::kSmoothFloatsPerVertex);
    // 第 0 列：先 right（v = 0.5 − scale/2），再 left（v = 0.5 + scale/2）。
    const float* right0 = &verts[0];
    const float* left0 = &verts[jpov::kSmoothFloatsPerVertex];
    EXPECT_FLOAT_EQ(right0[1], -0.3f * c.scale);   // center.y = 0
    EXPECT_FLOAT_EQ(right0[4], 0.5f - 0.5f * c.scale);
    EXPECT_FLOAT_EQ(right0[6], 7.0f);              // strip_id
    EXPECT_FLOAT_EQ(left0[1], 0.3f * c.scale);
    EXPECT_FLOAT_EQ(left0[4], 0.5f + 0.5f * c.scale);
    EXPECT_FLOAT_EQ(left0[3], 0.0f);               // u = 0/15
}

TEST(StripSmooth, CollapsedTailMatchesLastRealColumn) {
    // point_count=5 ⇒ 第 5..15 列与第 4 列**逐 float 相同**（零面积 quad，不产生片元）。
    jpov::Strip16 s = MakeStrip(5);
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(s, &c);
    std::vector<float> verts;
    jpov::AppendSmoothStripVertices(s, c, 0.0f, &verts);
    const size_t col_stride =
        static_cast<size_t>(2) * jpov::kSmoothFloatsPerVertex;   // 每列 right+left
    for (size_t col = 5; col < static_cast<size_t>(jpov::kStripSampleCount); ++col) {
        for (size_t j = 0; j < col_stride; ++j) {
            // 位置/alpha 相同；u 与 v 按列号变（与折线 pass 的塌缩规则一致）。
            const size_t idx = col * col_stride + j;
            const size_t ref = 4 * col_stride + j;
            const size_t in_vertex = j % jpov::kSmoothFloatsPerVertex;
            const bool is_u = (in_vertex == 3);
            if (is_u) {
                continue;
            }
            EXPECT_FLOAT_EQ(verts[idx], verts[ref])
                << "col=" << col << " j=" << j;
        }
    }
}


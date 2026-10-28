// JPOV 半透明条带：CPU 侧几何展开 + 输入校验的单测（GL-free，不需要 GL context）
//
// 覆盖点：
//   * 展开出的顶点数量/顺序（right 先、left 后）与 uv 约定（u=0/1、v=col/15）；
//   * 不足 16 点时的**尾部塌缩**（多余列坐在最后一个有效点上，且 uv 不跟着改）；
//   * 顶点**不再带 alpha**（已并进逐点颜色，由 FS 采样）；`Strip16{}` 的默认值是乘法单位元；
//   * 索引模式：每 quad 6 个索引、共 90 个、全部落在本块 [base, base+32) 内；
//   * ValidateStripBatch 的负向用例（point_count / point_color / profile / NaN / geometry）必须崩 ——
//     这些是「用户忘了填」的唯一防线，不能静默通过；
//   * **平滑 pass（kSmooth）的 CPU 前端**：切线（两端必须是完整单侧差分）、
//     自动外扩量（必须覆盖真实外凸量）、数据纹理打包顺序、顶点外扩与 v 的一致。
//     这组断言专门盯「出图看不出来」的错误，每一项都带**负向对照**（把旧实现算回来必须失败）。
//   * **圆环（四组点 → 四条带）**：四组点的高度/半径、闭合（15 段）、
//     「哪组点当左轨/右轨」的硬约定（宽度方向是径向还是高度）；
//   * **菲涅耳参数**的取值校验（非法值必须崩）。

#include <algorithm>
#include <array>
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
    }
    return s;
}

jpov::TranslucentStrip16Batch MakeBatch(uint32_t point_count) {
    jpov::TranslucentStrip16Batch b;
    b.strips.push_back(MakeStrip(point_count));
    return b;
}

// 4 采样点档：左轨 y=1、右轨 y=0，x = 列下标（与 MakeStrip 同一约定，便于对比）。
jpov::Strip4 MakeStrip4(uint32_t point_count) {
    jpov::Strip4 s;
    s.point_count = point_count;
    for (uint32_t i = 0; i < jpov::kStrip4SampleCount; ++i) {
        const float f = static_cast<float>(i);
        s.left[i] = jpov::Vec3f{f, 1.0f, 0.0f};
        s.right[i] = jpov::Vec3f{f, 0.0f, 0.0f};
    }
    return s;
}

jpov::TranslucentStrip4Batch MakeBatch4(uint32_t point_count) {
    jpov::TranslucentStrip4Batch b;
    b.strips.push_back(MakeStrip4(point_count));
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

TEST(StripGeometry, DefaultsAreIdentityMultipliers) {
    // `Strip16 s{}` 必须直接可用（2026-10-22 Danis 定）：逐点颜色 = 白 + 不透明、
    // 宽度剖面 = 全 1。这不是「特殊值语义」，是乘法单位元 ——
    // 改掉它就是让「不填」变成不可见，而不是「原样」。
    const jpov::Strip16 s{};
    for (const jpov::Color& c : s.point_color) {
        EXPECT_FLOAT_EQ(c.r, 1.0f);
        EXPECT_FLOAT_EQ(c.g, 1.0f);
        EXPECT_FLOAT_EQ(c.b, 1.0f);
        EXPECT_FLOAT_EQ(c.a, 1.0f);
    }
    for (float knot : s.width_profile) {
        EXPECT_FLOAT_EQ(knot, 1.0f);
    }
    EXPECT_EQ(s.point_count, 0u);   // 只有这个是「必须显式填」的
}

TEST(StripGeometry, VertexLayoutCarriesNoAlpha) {
    // 逐点 alpha 已并进逐点颜色（由 FS 从 per-instance 纹理采样），
    // 顶点只剩 pos + (u, v)：5 float / 顶点。
    EXPECT_EQ(sizeof(jpov::StripVertex), 5 * sizeof(float));
    EXPECT_EQ(jpov::kSmoothFloatsPerVertex, 6);   // + strip_id
}

TEST(StripGeometry, IndexPatternMatchesQuads) {
    // 第 0 条带（base=0）第 1 条带（base=32）各取一份，验证偏移与三角形组成。
    std::vector<uint32_t> idx;
    jpov::AppendStripIndicesForBlock(32, jpov::kStripQuads, &idx);
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
    const jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsZeroPointCount) {
    // 默认构造 = point_count 0（调用方忘了填）。必须崩，不能静默画一条空条带。
    jpov::TranslucentStrip16Batch batch;
    jpov::Strip16 strip = MakeStrip(16);
    strip.point_count = 0;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(StripGeometry, RejectsTooManyPoints) {
    jpov::TranslucentStrip16Batch batch;
    jpov::Strip16 strip = MakeStrip(16);
    strip.point_count = static_cast<uint32_t>(jpov::kStripSampleCount) + 1;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(StripGeometry, AcceptsSkyLightOnCoverage) {
    // 天光消费：合法用法（覆盖集 + 合法参数）必须**不崩**。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.sky_light = jpov::StripSkyLight{};
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsSkyLightOnEmissive) {
    // 加法集本身就是发光介质（注入量 = 色·α）；再叠「吸收→注入」会让 HDR 语义两套账。
    // 必须**崩**，不能静默忽略 —— 静默忽略等于骗调用方「设了但没生效」。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.blend = jpov::StripBlend::kEmissive;
    batch.sky_light = jpov::StripSkyLight{};
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "kCoverage");
}

TEST(StripGeometry, RejectsSkyLightInjectionOutOfRange) {
    // injection 是**物理比例**（吸收多少补多少）∈ [0,1]；> 1 会把「亮度旋钮」和
    // 「比例」两件事混在一起（想更亮应当放大天光色/强度）。越界必须崩。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    jpov::StripSkyLight sky;
    sky.injection = 1.5f;
    batch.sky_light = sky;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "injection");
}

TEST(StripGeometry, RejectsZeroSunDirection) {
    // 零向量会让 shader 里的 normalize 出 NaN，而 NaN 进 accum 会污染整帧的 WBO 解析。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    jpov::StripSkyLight sky;
    sky.sun_direction = jpov::Vec3f{0.0f, 0.0f, 0.0f};
    batch.sky_light = sky;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "sun_direction");
}

TEST(StripGeometry, RejectsHugeAmbientScale) {
    // 上界防御：把 lux 直接填进 ambient_scale（例如 20000）是常见单位错误，
    // 放行会让画面和 bloom 无声爆炸。必须崩。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    jpov::StripSkyLight sky;
    sky.ambient_scale = 20000.0f;
    batch.sky_light = sky;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "ambient_scale");
}

TEST(StripGeometry, RejectsNegativeSkyTricolor) {
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    jpov::StripSkyLight sky;
    sky.tricolor[1].r = -0.1f;
    batch.sky_light = sky;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "tricolor");
}

TEST(StripGeometry, RejectsAllZeroPointAlpha) {
    // 典型误用：整块 memset 0 之后忘了填颜色/alpha → 整批隐形。必须崩。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    for (jpov::Color& c : batch.strips[0].point_color) {
        c.a = 0.0f;
    }
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_color.a");
}

TEST(StripGeometry, RejectsAllZeroProfile) {
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.strips[0].width_profile.fill(0.0f);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "width_profile");
}

TEST(StripGeometry, RejectsNonFinitePosition) {
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.strips[0].left[3] = jpov::Vec3f{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "非有限值");
}

TEST(StripGeometry, RejectsOutOfRangePointColor) {
    // rgb 允许 HDR（> 1），但 a 仍是覆盖率（≤ 1）、rgb 不能为负也不能超过防御上限。
    jpov::TranslucentStrip16Batch alpha = MakeBatch(16);
    alpha.strips[0].point_color[0].a = 1.5f;
    EXPECT_DEATH(jpov::ValidateStripBatch(alpha), "point_color");

    jpov::TranslucentStrip16Batch negative = MakeBatch(16);
    negative.strips[0].point_color[0].r = -0.01f;
    EXPECT_DEATH(jpov::ValidateStripBatch(negative), "point_color");

    jpov::TranslucentStrip16Batch nan = MakeBatch(16);
    nan.strips[0].point_color[0].g = std::numeric_limits<float>::quiet_NaN();
    EXPECT_DEATH(jpov::ValidateStripBatch(nan), "point_color");

    jpov::TranslucentStrip16Batch too_bright = MakeBatch(16);
    too_bright.strips[0].point_color[0].b = jpov::kMaxStripColorValue + 1.0f;
    EXPECT_DEATH(jpov::ValidateStripBatch(too_bright), "point_color");
}

TEST(StripGeometry, AcceptsHdrPointColor) {
    // 加法集的亮度靠逐点颜色 > 1 表达（旧的 additive_gain 旋钮已删除）：
    // 这里必须**不崩**，否则「HDR 发光」这条路就被校验堵死了。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.blend = jpov::StripBlend::kEmissive;
    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        batch.strips[0].point_color[static_cast<size_t>(i)].r = 6.0f;
    }
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsNonPositiveSoftDepth) {
    // nullopt = 关闭；给了值就必须 > 0（0 会让淡出表达式除零）。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.soft_depth_m = 0.0f;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "soft_depth_m");
}

TEST(StripGeometry, RejectsIllegalBlendEnum) {
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.blend = static_cast<jpov::StripBlend>(7);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "blend");
}

TEST(StripGeometry, RejectsIllegalGeometryEnum) {
    // geometry 非法 cast 值不许静默走折线 pass。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.geometry = static_cast<jpov::StripInterpolation>(9);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "geometry");
}

TEST(StripGeometry, AcceptsSoftDepthAndEmissive) {
    // 合法组合必须不崩（执行到这里没 abort 就是通过）。
    jpov::TranslucentStrip16Batch batch = MakeBatch(16);
    batch.blend = jpov::StripBlend::kEmissive;
    batch.soft_depth_m = 0.15f;
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(StripGeometry, RejectsEmptyBatch) {
    jpov::TranslucentStrip16Batch batch;
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
    // 每列 16 float 的顺序必须与 kSmoothFsMain 的解包逐字对应：
    //   c.xyz, m.xyz, h.xyz, n.xyz, point_color.rgba
    // （改成 16 float = 4 texel 的原因：逐点颜色也进数据纹理，省掉平滑 pass 那次过滤采样）
    jpov::Strip16 s;
    s.point_count = 3;
    for (uint32_t i = 0; i < jpov::kStripSampleCount; ++i) {
        const float f = static_cast<float>(i);
        s.left[i] = jpov::Vec3f{f, f + 0.25f, -f};
        s.right[i] = jpov::Vec3f{f, f - 0.25f, -f};
        // 每列一个**互不相同**的颜色：只验证“有 4 个数”是不够的，
        // 颜色错列（比如把上一列的颜色打包进来）必须被抓住。
        s.point_color[i] = jpov::Color{f, f + 0.5f, -f, 0.25f};
    }
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(s, &c);
    std::vector<float> data;
    jpov::AppendSmoothStripData(s, c, &data);

    ASSERT_EQ(data.size(), static_cast<size_t>(jpov::kStripSampleCount) *
                               jpov::kSmoothFloatsPerColumn);
    EXPECT_EQ(jpov::kSmoothFloatsPerColumn, 16);
    EXPECT_EQ(jpov::kSmoothTexelsPerColumn, 4);
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
    // 逐点颜色是本列的（第 1 列）而不是邻列的：rgba = (1, 1.5, -1, 0.25)。
    EXPECT_FLOAT_EQ(data[base + 12], 1.0f);    // color.r
    EXPECT_FLOAT_EQ(data[base + 13], 1.5f);    // color.g
    EXPECT_FLOAT_EQ(data[base + 14], -1.0f);   // color.b
    EXPECT_FLOAT_EQ(data[base + 15], 0.25f);   // color.a

    // 第 0 列的颜色必须是 (0, 0.5, 0, 0.25)：防止“列号算错一位”整张纹理错色。
    EXPECT_FLOAT_EQ(data[12], 0.0f);
    EXPECT_FLOAT_EQ(data[13], 0.5f);
    EXPECT_FLOAT_EQ(data[14], 0.0f);
    EXPECT_FLOAT_EQ(data[15], 0.25f);

    // point_count=3 ⇒ 第 3 列起是塔缩列：颜色跟随最后一个有效列（否则后面是未初始化值）。
    const size_t collapse_base = 3 * jpov::kSmoothFloatsPerColumn;
    EXPECT_FLOAT_EQ(data[collapse_base + 12], 2.0f);
    EXPECT_FLOAT_EQ(data[collapse_base + 13], 2.5f);
    EXPECT_FLOAT_EQ(data[collapse_base + 14], -2.0f);
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
    EXPECT_FLOAT_EQ(right0[5], 7.0f);              // strip_id
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
            // 位置/v/strip_id 相同；u 按列号变（与折线 pass 的塌缩规则一致）。
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


// ────────────────── 圆环（四组点 → 四条带）──────────────────
//
// 这组断言盯的是「出图看着像个圈、其实接错了」的一类错误：
// 把 up/down 或 inner/outer 接反、把立壁的宽度方向当成径向、忘了闭合成 15 段 ——
// 画面都仍然是个圆圈，只有量坐标才看得出来。

namespace {

jpov::StripRingShape MakeTestRingShape() {
    jpov::StripRingShape shape;
    shape.outer_radius = 1.00f;
    shape.wall_thickness = 0.20f;
    shape.height = 1.50f;
    shape.center_x = 0.30f;
    shape.center_y = 2.00f;
    shape.center_z = -0.40f;
    shape.yaw = 0.0f;
    return shape;
}

// 到圆筒轴（竖直、过 center_x/center_z）的水平距离。
float AxisDistance(const jpov::Vec3f& p, const jpov::StripRingShape& shape) {
    const float dx = p.x() - shape.center_x;
    const float dz = p.z() - shape.center_z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

TEST(StripRing, FourPointRingsMatchShape) {
    const jpov::StripRingShape shape = MakeTestRingShape();
    const jpov::Ring16 ring = jpov::MakeRing16(shape);
    const float y_up = shape.center_y + 0.5f * shape.height;
    const float y_down = shape.center_y - 0.5f * shape.height;
    const float r_inner = shape.outer_radius - shape.wall_thickness;

    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        // 高度：上两组在上沿、下两组在下沿（up/down 接反立刻挂）。
        EXPECT_NEAR(ring.up_outer[k].y(), y_up, 1e-5f);
        EXPECT_NEAR(ring.up_inner[k].y(), y_up, 1e-5f);
        EXPECT_NEAR(ring.down_outer[k].y(), y_down, 1e-5f);
        EXPECT_NEAR(ring.down_inner[k].y(), y_down, 1e-5f);
        // 半径：outer/inner 各自对得上（内外接反也立刻挂）。
        EXPECT_NEAR(AxisDistance(ring.up_outer[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(AxisDistance(ring.up_inner[k], shape), r_inner, 1e-5f);
        EXPECT_NEAR(AxisDistance(ring.down_outer[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(AxisDistance(ring.down_inner[k], shape), r_inner, 1e-5f);
    }

    // 闭合：第 15 列 = 第 0 列（一圈只有 15 段；漏掉这条会留下一条缝）。
    EXPECT_NEAR(ring.up_outer[15].x(), ring.up_outer[0].x(), 1e-4f);
    EXPECT_NEAR(ring.up_outer[15].z(), ring.up_outer[0].z(), 1e-4f);
    EXPECT_NEAR(ring.down_inner[15].x(), ring.down_inner[0].x(), 1e-4f);
    EXPECT_NEAR(ring.down_inner[15].z(), ring.down_inner[0].z(), 1e-4f);

    // 第 0 / 15 列重合，但中间列必须真的分开（防止「整圈塌成一个点」也能过）。
    EXPECT_GT((ring.up_outer[8] - ring.up_outer[0]).Norm(), 1.0f);
}

TEST(StripRing, StripsPairRailsAsDocumented) {
    // 四条带的「哪组点当左轨/右轨」是硬约定：顶/底环面的宽度方向是**径向**，
    // 内外立壁的宽度方向是**高度**。把 width 方向搞错 = 圆环被画成 4 个平面。
    const jpov::StripRingShape shape = MakeTestRingShape();
    const std::array<jpov::Strip16, jpov::kStripRingPartCount> strips =
        jpov::MakeStripRing(shape);
    ASSERT_EQ(strips.size(), static_cast<size_t>(jpov::kStripRingPartCount));

    for (const jpov::Strip16& s : strips) {
        // 每条带都要 16 点（圆环的「一圈」不能少于 16 列）。
        EXPECT_EQ(s.point_count, static_cast<uint32_t>(jpov::kStripSampleCount));
        // 形状本身不带淡出/染色：默认逐点颜色（白 + 不透明）。
        for (const jpov::Color& c : s.point_color) {
            EXPECT_FLOAT_EQ(c.r, 1.0f);
            EXPECT_FLOAT_EQ(c.g, 1.0f);
            EXPECT_FLOAT_EQ(c.b, 1.0f);
            EXPECT_FLOAT_EQ(c.a, 1.0f);
        }
    }

    const jpov::Strip16& top = strips[static_cast<size_t>(jpov::StripRingPart::kTopFace)];
    const jpov::Strip16& bottom =
        strips[static_cast<size_t>(jpov::StripRingPart::kBottomFace)];
    const jpov::Strip16& outer =
        strips[static_cast<size_t>(jpov::StripRingPart::kOuterWall)];
    const jpov::Strip16& inner =
        strips[static_cast<size_t>(jpov::StripRingPart::kInnerWall)];

    const float r_inner = shape.outer_radius - shape.wall_thickness;
    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        // 顶环面：left = up_outer、right = up_inner ⇒ 宽度 = 环宽。
        EXPECT_NEAR((top.left[k] - top.right[k]).Norm(), shape.wall_thickness, 1e-5f);
        EXPECT_NEAR(AxisDistance(top.left[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(AxisDistance(top.right[k], shape), r_inner, 1e-5f);
        // 底环面同理。
        EXPECT_NEAR((bottom.left[k] - bottom.right[k]).Norm(), shape.wall_thickness, 1e-5f);
        // 立壁：宽度方向 = 高度（不是径向！）。
        EXPECT_NEAR((outer.left[k] - outer.right[k]).Norm(), shape.height, 1e-5f);
        EXPECT_NEAR((inner.left[k] - inner.right[k]).Norm(), shape.height, 1e-5f);
        EXPECT_NEAR(AxisDistance(outer.left[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(AxisDistance(outer.right[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(AxisDistance(inner.left[k], shape), r_inner, 1e-5f);
        EXPECT_NEAR(AxisDistance(inner.right[k], shape), r_inner, 1e-5f);
        // 立壁的左轨在上、右轨在下（否则法线/纹理 v 的方向整体翻转）。
        EXPECT_NEAR(outer.left[k].y(), shape.center_y + 0.5f * shape.height, 1e-5f);
        EXPECT_NEAR(outer.right[k].y(), shape.center_y - 0.5f * shape.height, 1e-5f);
        EXPECT_NEAR(inner.left[k].y(), shape.center_y + 0.5f * shape.height, 1e-5f);
        EXPECT_NEAR(inner.right[k].y(), shape.center_y - 0.5f * shape.height, 1e-5f);
    }
    // 同一条带的左右轨不能重合（否则宽度为 0，整条带不可见）。
    EXPECT_GT((top.left[0] - top.right[0]).Norm(), 0.0f);
    EXPECT_GT((outer.left[0] - outer.right[0]).Norm(), 0.0f);
}

TEST(StripRing, YawRotatesPointsWithoutChangingShape) {
    // yaw 只转朝向：半径/高度不变，但第 0 列不再落在 +x 轴上
    // （漏做旋转或旋转方向写反都会被抓到）。
    jpov::StripRingShape shape = MakeTestRingShape();
    const jpov::Ring16 base = jpov::MakeRing16(shape);
    shape.yaw = 0.7f;
    const jpov::Ring16 rotated = jpov::MakeRing16(shape);

    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        EXPECT_NEAR(AxisDistance(rotated.up_outer[k], shape), shape.outer_radius, 1e-5f);
        EXPECT_NEAR(rotated.up_outer[k].y(), base.up_outer[k].y(), 1e-5f);
        EXPECT_GT((rotated.up_outer[k] - base.up_outer[k]).Norm(), 1e-4f);
    }
    // 旋转是刚体变换：相邻列间距不变。
    const float base_step = (base.up_outer[1] - base.up_outer[0]).Norm();
    const float rot_step = (rotated.up_outer[1] - rotated.up_outer[0]).Norm();
    EXPECT_NEAR(rot_step, base_step, 1e-5f);
}

TEST(StripRing, RejectsInvalidShape) {
    // 壁厚 ≥ 外半径 → 内半径 ≤ 0（几何自交）；高度 0 → 立壁退化成线。
    jpov::StripRingShape shape = MakeTestRingShape();
    shape.wall_thickness = shape.outer_radius;
    EXPECT_DEATH(jpov::MakeRing16(shape), "wall_thickness");

    jpov::StripRingShape flat = MakeTestRingShape();
    flat.height = 0.0f;
    EXPECT_DEATH(jpov::MakeRing16(flat), "height");
}

TEST(StripGeometry, RejectsBadFresnelParams) {
    // 菲涅耳参数是「设了就要生效」的：非法值必须崩，不能静默画出一个奇怪的边。
    jpov::TranslucentStrip16Batch ok = MakeBatch(16);
    ok.fresnel = jpov::StripFresnel{};
    jpov::ValidateStripBatch(ok);   // 合法值不崩

    jpov::TranslucentStrip16Batch f0 = MakeBatch(16);
    f0.fresnel = jpov::StripFresnel{};
    f0.fresnel->f0 = 1.5f;   // 正入射反射率不可能 > 1
    EXPECT_DEATH(jpov::ValidateStripBatch(f0), "fresnel.f0");

    jpov::TranslucentStrip16Batch power = MakeBatch(16);
    power.fresnel = jpov::StripFresnel{};
    power.fresnel->power = 0.0f;   // 0 次幂 = 恒 1（整面全反），几乎必然是忘填
    EXPECT_DEATH(jpov::ValidateStripBatch(power), "fresnel.power");

    jpov::TranslucentStrip16Batch strength = MakeBatch(16);
    strength.fresnel = jpov::StripFresnel{};
    strength.fresnel->strength = -0.5f;
    EXPECT_DEATH(jpov::ValidateStripBatch(strength), "fresnel.strength");

    jpov::TranslucentStrip16Batch graze = MakeBatch(16);
    graze.fresnel = jpov::StripFresnel{};
    graze.fresnel->graze_alpha = 1.2f;
    EXPECT_DEATH(jpov::ValidateStripBatch(graze), "fresnel.graze_alpha");

    jpov::TranslucentStrip16Batch rim = MakeBatch(16);
    rim.fresnel = jpov::StripFresnel{};
    rim.fresnel->rim_color = jpov::Vec3f{-0.1f, 0.0f, 0.0f};
    EXPECT_DEATH(jpov::ValidateStripBatch(rim), "fresnel.rim_color");
}


// ────────────────── 4 采样点档（Strip4）──────────────────
//
// 这一组盯的是「同一套算法换一个采样数」有没有走偏：
//   * 拓扑常量（8 顶点 / 18 索引 / 行宽 13）与 16 点档的对应关系；
//   * 尾部塌缩、UV 约定与 16 点档**逐字一致**（u = col/quads，v 仍是 0/1）；
//   * 校验规则与 16 点档相同，只是上限是 4（不是「4 点档可以随便填」）；
//   * 平滑前端：直线条带在 4 点档下同样不外扩、2 点条带同样退化成折线；
//   * 数据纹理行：4 点档只写前 4 列，其余补 0 到 16 点档的行跨距。

TEST(Strip4, TopologyConstantsMatchLayout) {
    EXPECT_EQ(jpov::kStrip4SampleCount, 4);
    EXPECT_EQ(jpov::kStrip4Quads, 3);
    EXPECT_EQ(jpov::kStripLayout4.samples, 4);
    EXPECT_EQ(jpov::kStripLayout4.quads, 3);
    EXPECT_EQ(jpov::kStripLayout4.vertices_per_strip, 8);
    EXPECT_EQ(jpov::kStripLayout4.indices_per_strip, 18);
    EXPECT_EQ(jpov::kStripLayout4.appearance_texels, 13);   // 4 色 + 9 剖面 knot
    // 与 16 点档的关系：顶点/索引是 1/4，profile 段数不变。
    EXPECT_EQ(jpov::kStripLayout16.vertices_per_strip,
              jpov::kStripLayout4.vertices_per_strip * 4);
    EXPECT_EQ(jpov::kStripLayout16.indices_per_strip,
              jpov::kStripLayout4.indices_per_strip * 5);   // 90 = 18 × 5（15 段 vs 3 段）
}

TEST(Strip4, ExpandsFixedTopology) {
    std::vector<jpov::StripVertex> verts;
    jpov::ExpandStripVertices(MakeStrip4(4), &verts);
    ASSERT_EQ(verts.size(), 8u);

    // 第 0 列：先 right（u=0, v=0），再 left（u=0, v=1）——与 16 点档同一顺序。
    EXPECT_FLOAT_EQ(verts[0].v, 0.0f);
    EXPECT_FLOAT_EQ(verts[1].v, 1.0f);
    EXPECT_FLOAT_EQ(verts[0].u, 0.0f);
    // 第 1 列 u = 1/3，第 3 列 u = 1（纹理沿长度仍是「整段铺满」）。
    EXPECT_NEAR(verts[2].u, 1.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(verts[6].u, 1.0f, 1e-6f);
    // 位置来自各自的 left/right：x = 列下标，左轨 y=1、右轨 y=0。
    EXPECT_FLOAT_EQ(verts[6].position.x(), 3.0f);
    EXPECT_FLOAT_EQ(verts[7].position.y(), 1.0f);
}

TEST(Strip4, CollapsesTailColumns) {
    // 3 点时第 3 列（最后一列）必须塌缩到第 2 点上，但 **u 不变**（= 1）：
    // 零面积 quad 不产生片元，同时短条带不会把纹理拉伸。
    std::vector<jpov::StripVertex> short_strip;
    jpov::ExpandStripVertices(MakeStrip4(3), &short_strip);
    ASSERT_EQ(short_strip.size(), 8u);
    EXPECT_FLOAT_EQ(short_strip[4].position.x(), 2.0f);   // 第 2 列 = 有效点 2
    EXPECT_FLOAT_EQ(short_strip[6].position.x(), 2.0f);   // 第 3 列塌缩到同一点
    EXPECT_FLOAT_EQ(short_strip[6].u, 1.0f);              // 但 u 仍是 1
}

TEST(Strip4, IndexPatternMatchesQuads) {
    std::vector<uint32_t> idx;
    jpov::AppendStripIndicesForBlock(8, jpov::kStripLayout4.quads, &idx);
    ASSERT_EQ(idx.size(), 18u);
    // 第一个 quad：right0(8) left0(9) right1(10) / left0(9) left1(11) right1(10)
    EXPECT_EQ(idx[0], 8u);
    EXPECT_EQ(idx[1], 9u);
    EXPECT_EQ(idx[2], 10u);
    EXPECT_EQ(idx[3], 9u);
    EXPECT_EQ(idx[4], 11u);
    EXPECT_EQ(idx[5], 10u);
    for (uint32_t v : idx) {
        EXPECT_GE(v, 8u);
        EXPECT_LT(v, 16u);
    }
    EXPECT_EQ(idx[17], 14u);   // 最后一个 quad 的 right_{q+1}
}

TEST(Strip4, ValidBatchPasses) {
    const jpov::TranslucentStrip4Batch batch = MakeBatch4(4);
    jpov::ValidateStripBatch(batch);
    SUCCEED();
}

TEST(Strip4, RejectsZeroPointCount) {
    jpov::TranslucentStrip4Batch batch;
    jpov::Strip4 strip = MakeStrip4(4);
    strip.point_count = 0;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(Strip4, RejectsTooManyPoints) {
    // 上限是**本档位的容量**（4），不是 16：写 5 必须崩 —— 否则会静默读到数组外。
    jpov::TranslucentStrip4Batch batch;
    jpov::Strip4 strip = MakeStrip4(4);
    strip.point_count = 5;
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "point_count");
}

TEST(Strip4, RejectsAllTransparentStrip) {
    // 与 16 点档同一条防线：逐点 alpha 全 0 = 这条带永远看不见，必须崩而不是静默。
    jpov::TranslucentStrip4Batch batch;
    jpov::Strip4 strip = MakeStrip4(4);
    for (jpov::Color& c : strip.point_color) {
        c.a = 0.0f;
    }
    batch.strips.push_back(strip);
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "a 全为 0");
}

TEST(Strip4, SmoothStraightStripIsNotWidened) {
    // 直条带：真实曲线 == 弦 ⇒ 外扩量只剩「2% + 1mm」的固定余量（不是 0），
    // 于是 scale = 1 + 0.001 / 最细半宽。这里半宽恒为 0.5 ⇒ 1.002。
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(MakeStrip4(4), &c);
    EXPECT_NEAR(c.margin, 0.001f, 1e-6f);
    EXPECT_NEAR(c.scale, 1.0f + 0.001f / 0.5f, 1e-5f);
}

TEST(Strip4, SmoothTwoPointEqualsPolyline) {
    // 只有 2 个有效点时，3 段里有两段是塌缩的零面积 quad：
    // 顶点必须落在「折线顶点沿半宽方向外扩 scale 倍」的位置上（FS 会把它裁回真实轮廓），
    // 也就是「2 点 kSmooth == kPolyline」在浮点意义上成立。
    const jpov::Strip4 strip = MakeStrip4(2);
    std::vector<jpov::StripVertex> polyline;
    jpov::ExpandStripVertices(strip, &polyline);

    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(strip, &c);
    std::vector<float> smooth;
    jpov::AppendSmoothStripVertices(strip, c, 0.0f, &smooth);
    ASSERT_EQ(smooth.size(), static_cast<size_t>(8 * jpov::kSmoothFloatsPerVertex));

    const float v_right = 0.5f - 0.5f * c.scale;
    const float v_left = 0.5f + 0.5f * c.scale;
    for (int col = 0; col < 4; ++col) {
        for (int side = 0; side < 2; ++side) {
            const size_t vi = static_cast<size_t>(col * 2 + side);
            const jpov::StripVertex& p = polyline[vi];
            // 中心 = 该列 left/right 的中点（这里是 y = 0.5）；外扩沿半宽方向。
            const float cy = 0.5f;
            const float expected_y = cy + (p.position.y() - cy) * c.scale;
            EXPECT_NEAR(smooth[vi * 6 + 0], p.position.x(), 1e-5f);
            EXPECT_NEAR(smooth[vi * 6 + 1], expected_y, 1e-5f);
            EXPECT_NEAR(smooth[vi * 6 + 2], p.position.z(), 1e-5f);
            EXPECT_NEAR(smooth[vi * 6 + 3], p.u, 1e-6f);
            EXPECT_NEAR(smooth[vi * 6 + 4], side == 0 ? v_right : v_left, 1e-6f);
        }
    }
    // 反向对照：外扩量只有 1mm 级别 ⇒ 与折线顶点的偏差必须 < 2mm，
    // 若哪天 margin 算成「按曲率的外凸量」忘了回零，这条会立刻红。
    EXPECT_LT(std::fabs(smooth[1] - polyline[0].position.y()), 0.002f);
}

TEST(Strip4, SmoothDataRowIsPaddedToSharedStride) {
    // 两种档位共用一张数据纹理（行跨距按 16 点档 = 256 float）。
    // 4 点档必须只写前 64 个 float（4 列 × 16），其余补 0 —— 少写会读到上一行的脏数据。
    const jpov::Strip4 strip = MakeStrip4(4);
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(strip, &c);

    std::vector<float> same_stride;
    jpov::AppendSmoothStripData(strip, /*row_stride_floats=*/64, c, &same_stride);
    EXPECT_EQ(same_stride.size(), 64u);   // 恰好一行（4 × 16），没有多余填充

    std::vector<float> padded;
    jpov::AppendSmoothStripData(strip, /*row_stride_floats=*/256, c, &padded);
    ASSERT_EQ(padded.size(), 256u);
    for (size_t i = 64; i < padded.size(); ++i) {
        EXPECT_FLOAT_EQ(padded[i], 0.0f) << "填充位必须是 0，i=" << i;
    }
    // 前 64 个 float 与「不填充」版本逐字相同（填充不该影响有效数据）。
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_FLOAT_EQ(padded[i], same_stride[i]);
    }
}

TEST(Strip4, SmoothDataRejectsTooSmallStride) {
    // 行跨距小于本布局需要 = 会覆盖下一行：必须崩，不能静默写坏纹理。
    const jpov::Strip4 strip = MakeStrip4(4);
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(strip, &c);
    std::vector<float> out;
    EXPECT_DEATH(jpov::AppendSmoothStripData(strip, /*row_stride_floats=*/63, c, &out),
                 "行跨距");
}

// ────────────────── 四档采样数（4 / 8 / 16 / 32）的统一检查 ──────────────────
//
// 只有「档位表」本身值得逐项钉住：其余数学（塌缩 / 切线 / 外扩 / 打包）已经是
// view + layout 驱动的**同一份实现**，四档走的是同一段代码，分别写四遍测试只会
// 复制粘贴。这里用 StripSamplesView（运行期视图）把四档串成一张表。

namespace {

// 一张「最大档」的临时采样数据：左轨 y=1、右轨 y=0、x = 列下标（与 MakeStrip 同一约定）。
struct ScratchStrip {
    jpov::Vec3f left[jpov::kStripMaxSampleCount];
    jpov::Vec3f right[jpov::kStripMaxSampleCount];
    jpov::Color color[jpov::kStripMaxSampleCount];
    std::array<float, jpov::kStripProfileKnots> profile = jpov::kFlatWidthProfile;

    explicit ScratchStrip(uint32_t point_count) {
        for (int i = 0; i < jpov::kStripMaxSampleCount; ++i) {
            const float f = static_cast<float>(i);
            left[i] = jpov::Vec3f{f, 1.0f, 0.0f};
            right[i] = jpov::Vec3f{f, 0.0f, 0.0f};
            color[i] = jpov::Color{1.0f, 1.0f, 1.0f, 1.0f};
        }
        view.point_count = point_count;
    }

    // capacity 由调用方按档位给（4/8/16/32）：视图本身不知道自己是哪一档。
    jpov::StripSamplesView View(int capacity) {
        view.left = left;
        view.right = right;
        view.point_color = color;
        view.width_profile = &profile;
        view.capacity = static_cast<uint32_t>(capacity);
        return view;
    }

private:
    jpov::StripSamplesView view{};
};

const jpov::StripLayout kAllLayouts[] = {jpov::kStripLayout4, jpov::kStripLayout8,
                                         jpov::kStripLayout16, jpov::kStripLayout32};

}  // namespace

TEST(StripVariants, TopologyTableIsConsistent) {
    // 顶点/索引/行宽必须都能由采样数推出来（渲染器与 shader 的 uniform 全靠这张表）。
    const int kExpectedSamples[] = {4, 8, 16, 32};
    const int kExpectedTexels[] = {13, 17, 25, 41};
    for (size_t i = 0; i < 4; ++i) {
        const jpov::StripLayout& l = kAllLayouts[i];
        EXPECT_EQ(l.samples, kExpectedSamples[i]);
        EXPECT_EQ(l.quads, l.samples - 1);
        EXPECT_EQ(l.vertices_per_strip, 2 * l.samples);
        EXPECT_EQ(l.indices_per_strip, l.quads * 6);
        EXPECT_EQ(l.appearance_texels, kExpectedTexels[i]);
        EXPECT_EQ(l.appearance_texels, l.samples + jpov::kStripProfileKnots);
        // 槽位映射与资源数组（四档各一套）必须一一对应。
        EXPECT_EQ(jpov::StripLayoutSlotOf(l.samples), static_cast<int>(i));
    }
    EXPECT_EQ(jpov::StripLayoutSlotOf(7), -1);   // 非法采样数：调用方 CHECK 崩
}

TEST(StripVariants, ExpandsEveryVariant) {
    for (const jpov::StripLayout& layout : kAllLayouts) {
        ScratchStrip scratch(static_cast<uint32_t>(layout.samples));
        std::vector<jpov::StripVertex> verts;
        jpov::ExpandStripVertices(scratch.View(layout.samples), layout, &verts);
        ASSERT_EQ(verts.size(), static_cast<size_t>(layout.vertices_per_strip))
            << "samples=" << layout.samples;
        // 每列两个顶点：right（v=0）在前、left（v=1）在后；u = col/quads。
        for (int col = 0; col < layout.samples; ++col) {
            const size_t base = static_cast<size_t>(col) * 2;
            EXPECT_FLOAT_EQ(verts[base].v, 0.0f);
            EXPECT_FLOAT_EQ(verts[base + 1].v, 1.0f);
            const float expected_u = static_cast<float>(col) /
                                     static_cast<float>(layout.samples - 1);
            EXPECT_NEAR(verts[base].u, expected_u, 1e-6f);
            EXPECT_NEAR(verts[base + 1].u, expected_u, 1e-6f);
        }
        EXPECT_NEAR(verts[verts.size() - 1].u, 1.0f, 1e-6f);   // 末尾列 u 必须是 1
    }
}

TEST(StripVariants, IndexPatternCoversEveryVertex) {
    for (const jpov::StripLayout& layout : kAllLayouts) {
        std::vector<uint32_t> idx;
        jpov::AppendStripIndicesForBlock(/*base_vertex=*/0, layout.quads, &idx);
        ASSERT_EQ(idx.size(), static_cast<size_t>(layout.indices_per_strip))
            << "samples=" << layout.samples;
        for (uint32_t v : idx) {
            EXPECT_LT(v, static_cast<uint32_t>(layout.vertices_per_strip));
        }
        // 最后一个 quad 的第二个三角形末顶点 = 最后一条「右轨」顶点（2*(quads+1)-2）。
        EXPECT_EQ(idx.back() + 1u,
                  static_cast<uint32_t>(layout.vertices_per_strip) - 1u);
    }
}

TEST(StripVariants, ValidateAcceptsFullAndCollapsed) {
    // 满点与塌缩（point_count = 2）都必须通过：塌缩是合法用法（2 点 = 直线）。
    for (const jpov::StripLayout& layout : kAllLayouts) {
        for (uint32_t points : {2u, static_cast<uint32_t>(layout.samples)}) {
            ScratchStrip scratch(points);
            jpov::ValidateStripSamples(scratch.View(layout.samples), /*index=*/0,
                                      "Scratch");
        }
    }
    SUCCEED();
}

TEST(StripVariants, ValidateRejectsOverCapacity) {
    // 超出**本档容量**一个点就必须崩（写成 32 会让小档位读越界）。
    for (const jpov::StripLayout& layout : kAllLayouts) {
        ScratchStrip scratch(static_cast<uint32_t>(layout.samples) + 1u);
        jpov::StripSamplesView view = scratch.View(layout.samples);
        EXPECT_DEATH(jpov::ValidateStripSamples(view, /*index=*/0, "Scratch"), "point_count")
            << "samples=" << layout.samples;
    }
}

TEST(StripVariants, SmoothStraightStripNotWidenedInEveryVariant) {
    // 直条带在四档都不该被外扩（只剩固定的 1mm + 2% 余量），
    // 且平滑前端不能因为档位变大而算错（数组按最大档分配，只有前 layout.samples 项有效）。
    for (const jpov::StripLayout& layout : kAllLayouts) {
        ScratchStrip scratch(static_cast<uint32_t>(layout.samples));
        jpov::SmoothStripCompute computed;
        jpov::ComputeSmoothStrip(scratch.View(layout.samples), layout, &computed);
        EXPECT_NEAR(computed.margin, 0.001f, 1e-6f) << "samples=" << layout.samples;
        // 半宽恒为 0.5 ⇒ scale = 1 + 0.001/0.5。
        EXPECT_NEAR(computed.scale, 1.0f + 0.001f / 0.5f, 1e-5f)
            << "samples=" << layout.samples;

        std::vector<float> verts;
        jpov::AppendSmoothStripVertices(scratch.View(layout.samples), layout, computed,
                                        0.0f, &verts);
        EXPECT_EQ(verts.size(), static_cast<size_t>(layout.vertices_per_strip *
                                                    jpov::kSmoothFloatsPerVertex))
            << "samples=" << layout.samples;
    }
}

TEST(StripVariants, SmoothDataRowPaddingMatchesStride) {
    // 四档共用同一张控制点数据纹理（行跨距 = 最大档 512 float）：
    // 小档位必须补 0 到行尾，且有效区逐字等于「不填充」版本。
    const int stride = jpov::kStripMaxSampleCount * jpov::kSmoothFloatsPerColumn;
    for (const jpov::StripLayout& layout : kAllLayouts) {
        ScratchStrip scratch(static_cast<uint32_t>(layout.samples));
        jpov::SmoothStripCompute computed;
        jpov::ComputeSmoothStrip(scratch.View(layout.samples), layout, &computed);

        std::vector<float> tight;
        jpov::AppendSmoothStripData(scratch.View(layout.samples), layout, computed,
                                    layout.samples * jpov::kSmoothFloatsPerColumn, &tight);
        std::vector<float> padded;
        jpov::AppendSmoothStripData(scratch.View(layout.samples), layout, computed,
                                    stride, &padded);

        ASSERT_EQ(padded.size(), static_cast<size_t>(stride)) << "samples=" << layout.samples;
        ASSERT_EQ(tight.size(), static_cast<size_t>(layout.samples * jpov::kSmoothFloatsPerColumn));
        for (size_t i = 0; i < tight.size(); ++i) {
            EXPECT_FLOAT_EQ(padded[i], tight[i]) << "samples=" << layout.samples;
        }
        for (size_t i = tight.size(); i < padded.size(); ++i) {
            EXPECT_FLOAT_EQ(padded[i], 0.0f) << "samples=" << layout.samples;
        }
    }
}

// ────────────────── 退化输入（合法但几何退化）──────────────────
//
// Danis 的问题：「万一用户输入了奇异数值（不含 NaN/Inf），比如**重叠点**，会不会让
// renderer 或者内部除以 0？」这些用例就是答案的 CPU 半边：
// 这些输入**能通过 ValidateStripBatch**（它们不是错误，只是几何退化了），所以每一处
// 除法/归一化都必须自己扛住。判据统一是「输出有限」+「不崩」，不假设某个具体数值。
//
// 为什么不在 ValidateStripBatch 里拒绝它们：重叠点/零宽是**合法的表达手段**
// （零宽 = 一条线、重合点 = 零长度段），拒绝等于把用户的合法几何判成非法。

namespace {

bool AllFinite(const jpov::Vec3f& v) {
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

// 把一条 16 点条带按「退化配方」填好，然后跑完整条 CPU 前端并检查每个输出都是有限值。
void CheckDegenerateFinite(const char* name, const jpov::Strip16& strip) {
    const jpov::StripSamplesView view = jpov::ViewOf(strip);
    // ① 校验必须**接受**（退化 ≠ 非法）。
    jpov::ValidateStripSamples(view, 0, name);

    // ② 平滑前端：切线 / 外扩量 / 打包。
    jpov::SmoothStripCompute computed;
    jpov::ComputeSmoothStrip(view, jpov::kStripLayout16, &computed);
    EXPECT_TRUE(std::isfinite(computed.margin)) << name;
    EXPECT_TRUE(std::isfinite(computed.scale)) << name;
    EXPECT_GE(computed.scale, 1.0f) << name;
    EXPECT_LE(computed.scale, jpov::kSmoothMaxWidenScale + 1e-5f) << name;
    for (int i = 0; i < jpov::kStripLayout16.samples; ++i) {
        EXPECT_TRUE(AllFinite(computed.center[i])) << name << " center[" << i << "]";
        EXPECT_TRUE(AllFinite(computed.half[i])) << name << " half[" << i << "]";
        EXPECT_TRUE(AllFinite(computed.center_tan[i])) << name << " center_tan[" << i << "]";
        EXPECT_TRUE(AllFinite(computed.half_tan[i])) << name << " half_tan[" << i << "]";
    }

    // ③ 数据行（控制点纹理）与两种顶点展开。
    std::vector<float> data;
    jpov::AppendSmoothStripData(view, jpov::kStripLayout16, computed,
                                jpov::kStripMaxSampleCount * jpov::kSmoothFloatsPerColumn,
                                &data);
    for (float v : data) {
        EXPECT_TRUE(std::isfinite(v)) << name << " smooth data";
    }
    std::vector<float> smooth_verts;
    jpov::AppendSmoothStripVertices(view, jpov::kStripLayout16, computed, 0.0f,
                                    &smooth_verts);
    for (float v : smooth_verts) {
        EXPECT_TRUE(std::isfinite(v)) << name << " smooth vertex";
    }
    std::vector<jpov::StripVertex> poly_verts;
    jpov::ExpandStripVertices(view, jpov::kStripLayout16, &poly_verts);
    ASSERT_EQ(poly_verts.size(), static_cast<size_t>(jpov::kStripVerticesPerStrip)) << name;
    for (const jpov::StripVertex& v : poly_verts) {
        EXPECT_TRUE(AllFinite(v.position)) << name << " polyline position";
        EXPECT_TRUE(std::isfinite(v.u) && std::isfinite(v.v)) << name << " polyline uv";
    }
}

// 16 点条带的退化配方：按几何关系逐列生成。
enum class DegenerateKind {
    kAllIdenticalZeroWidth,  // 所有点重合 + 零宽（退化成一个点）
    kAllIdenticalFullWidth,  // 所有点重合、但有宽度（退化成一条线段）
    kDuplicateTail,          // 后半段所有列都坐在同一个点上
    kZeroWidthCurve,         // 曲线正常、宽度恒为 0（一条线）
    kHourglass,              // 中间一列宽度为 0
    kSubMicron,              // 相邻列相距 1e-7 m
    kFoldBack,               // 180° 折返（后半段沿原路返回）
    kWedge,                  // 首列宽度 0，往后张开
    kFarAway,                // 坐标 1e6 m
};

jpov::Strip16 MakeDegenerate(DegenerateKind kind) {
    jpov::Strip16 s;
    constexpr int n = jpov::kStripSampleCount;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        jpov::Vec3f left{};
        jpov::Vec3f right{};
        switch (kind) {
            case DegenerateKind::kAllIdenticalZeroWidth:
                left = jpov::Vec3f{0.0f, 0.0f, 0.0f};
                right = left;
                break;
            case DegenerateKind::kAllIdenticalFullWidth:
                left = jpov::Vec3f{0.0f, 1.0f, 0.0f};
                right = jpov::Vec3f{0.0f, 0.0f, 0.0f};
                break;
            case DegenerateKind::kDuplicateTail: {
                const float x = (i < n / 2) ? static_cast<float>(i)
                                            : static_cast<float>(n / 2);
                left = jpov::Vec3f{x, 1.0f, 0.0f};
                right = jpov::Vec3f{x, 0.0f, 0.0f};
                break;
            }
            case DegenerateKind::kZeroWidthCurve: {
                const float x = -2.0f + 4.0f * t;
                left = jpov::Vec3f{x, 0.5f + 0.2f * std::sin(3.0f * t), 0.0f};
                right = left;
                break;
            }
            case DegenerateKind::kHourglass: {
                const float x = -2.0f + 4.0f * t;
                const float half = (i == n / 2) ? 0.0f : 0.3f;
                left = jpov::Vec3f{x, half, 0.0f};
                right = jpov::Vec3f{x, -half, 0.0f};
                break;
            }
            case DegenerateKind::kSubMicron: {
                const float x = 1e-7f * static_cast<float>(i);
                left = jpov::Vec3f{x, 1e-7f, 0.0f};
                right = jpov::Vec3f{x, -1e-7f, 0.0f};
                break;
            }
            case DegenerateKind::kFoldBack: {
                const int k = (i < n / 2) ? i : (n - 1 - i);
                const float x = static_cast<float>(k);
                left = jpov::Vec3f{x, 0.2f, 0.0f};
                right = jpov::Vec3f{x, -0.2f, 0.0f};
                break;
            }
            case DegenerateKind::kWedge: {
                const float x = -2.0f + 2.0f * t;
                const float half = 0.3f * t;   // t=0 时恰为 0
                left = jpov::Vec3f{x, half, 0.0f};
                right = jpov::Vec3f{x, -half, 0.0f};
                break;
            }
            case DegenerateKind::kFarAway: {
                const float x = 1e6f + static_cast<float>(i);
                left = jpov::Vec3f{x, 1e6f + 0.2f, 1e6f};
                right = jpov::Vec3f{x, 1e6f - 0.2f, 1e6f};
                break;
            }
        }
        s.left[static_cast<size_t>(i)] = left;
        s.right[static_cast<size_t>(i)] = right;
    }
    s.point_count = static_cast<uint32_t>(n);
    return s;
}

}  // namespace

TEST(StripDegenerate, AllCpuOutputsStayFinite) {
    const DegenerateKind kKinds[] = {
        DegenerateKind::kAllIdenticalZeroWidth, DegenerateKind::kAllIdenticalFullWidth,
        DegenerateKind::kDuplicateTail,         DegenerateKind::kZeroWidthCurve,
        DegenerateKind::kHourglass,             DegenerateKind::kSubMicron,
        DegenerateKind::kFoldBack,              DegenerateKind::kWedge,
        DegenerateKind::kFarAway,
    };
    for (DegenerateKind kind : kKinds) {
        CheckDegenerateFinite("degenerate", MakeDegenerate(kind));
    }
}

TEST(StripDegenerate, TwoIdenticalPointsStayFinite) {
    // 只给 2 个点、且两点完全重合：塌缩 + Hermite 全都退化，但必须仍然有限。
    jpov::Strip16 s = MakeDegenerate(DegenerateKind::kAllIdenticalFullWidth);
    s.point_count = 2;
    CheckDegenerateFinite("two-identical", s);
}

TEST(StripDegenerate, AllZeroWidthCollapsesToLine) {
    // 零宽的曲线：外扩量应退回「只剩固定余量」（没有任何"外侧"可撑），
    // 且 scale 必须是 1（min_half 全是 0 ⇒ 没有可用的半宽基准）。
    jpov::Strip16 s = MakeDegenerate(DegenerateKind::kZeroWidthCurve);
    jpov::SmoothStripCompute c;
    jpov::ComputeSmoothStrip(jpov::ViewOf(s), jpov::kStripLayout16, &c);
    EXPECT_FLOAT_EQ(c.scale, 1.0f);
    EXPECT_NEAR(c.margin, 0.001f, 1e-6f);
}

TEST(StripDegenerate, RingWithTinyWallAndHeightStaysFinite) {
    // 圆环：壁厚 1e-7、高 1e-6 —— 内外立壁几乎重合，仍是合法形状（只需 wall < outer）。
    jpov::StripRingShape shape;
    shape.outer_radius = 1.0f;
    shape.wall_thickness = 1e-7f;
    shape.height = 1e-6f;
    shape.center_y = 0.5f;
    const std::array<jpov::Strip16, jpov::kStripRingPartCount> ring =
        jpov::MakeStripRing(shape);
    for (const jpov::Strip16& strip : ring) {
        CheckDegenerateFinite("ring", strip);
    }
}

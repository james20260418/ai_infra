// JPOV 半透明条带：CPU 侧几何展开 + 输入校验的单测（GL-free，不需要 GL context）
//
// 覆盖点：
//   * 展开出的顶点数量/顺序（right 先、left 后）与 uv 约定（u=0/1、v=col/15）；
//   * 不足 16 点时的**尾部塌缩**（多余列坐在最后一个有效点上，且 uv 不跟着改）；
//   * 逐点 alpha 正确地带到两侧顶点；
//   * 索引模式：每 quad 6 个索引、共 90 个、全部落在本块 [base, base+32) 内；
//   * ValidateStripBatch 的负向用例（point_count / alpha / profile / tint / NaN）必须崩 ——
//     这些是「用户忘了填」的唯一防线，不能静默通过。

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

    // 第 0 列：先 right（u=0,v=0），再 left（u=1,v=0）。
    EXPECT_FLOAT_EQ(verts[0].position.y(), 0.0f);
    EXPECT_FLOAT_EQ(verts[0].u, 0.0f);
    EXPECT_FLOAT_EQ(verts[0].v, 0.0f);
    EXPECT_FLOAT_EQ(verts[1].position.y(), 1.0f);
    EXPECT_FLOAT_EQ(verts[1].u, 1.0f);
    EXPECT_FLOAT_EQ(verts[1].v, 0.0f);

    // 最后一列 col=15：v = 15/15 = 1。
    const jpov::StripVertex& last_right = verts[30];
    const jpov::StripVertex& last_left = verts[31];
    EXPECT_FLOAT_EQ(last_right.v, 1.0f);
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
        EXPECT_FLOAT_EQ(verts[2 * col].v,
                        static_cast<float>(col) / static_cast<float>(jpov::kStripQuads));
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

TEST(StripGeometry, RejectsEmptyBatch) {
    jpov::TranslucentStripBatch batch;
    EXPECT_DEATH(jpov::ValidateStripBatch(batch), "不能为空");
}

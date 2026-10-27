// 水面 quad 几何 / 材质参数的单测（纯 CPU，无 GL）。
//
// 锁的是 skylight_water_quad.h 的三条契约：
//   ① 平铺没有缝也没有重叠：相邻两条带**共用同一条边**（行 r 的 left 轨 == 行 r+1 的 right 轨），
//      整块面的拼起来面积 == 10 km²（不是「差不多大」）；
//   ② 条带的方向约定不被写反：沿 X 从左到右、right 轨在高 z 侧（v = 0）、left 在低 z 侧（v = 1）；
//   ③ 材质是玻璃而不是发光：覆盖集 + 折线几何 + 开了 fresnel，且逐点颜色 = 统一的水体色。
//
// 这些断言都能**真的失败**（把 BuildWaterQuadStrips 的行宽写成 side/(rows-1)、
// 或把 left/right 对调、或把 blend 改成 kEmissive，对应用例就会红）。

#include <cmath>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/demo/skylight_water_quad.h"

namespace {

// 浮点比较容差：坐标量级 ~1.6e3 m，float 尾数精度约 1e-4 m，留 100× 余量。
constexpr float kEpsPos = 1.0e-2f;

TEST(SkylightWaterQuad, TileCountAndSampleCount) {
    const std::vector<jpov::Strip32> strips = jpov_skylight::BuildWaterQuadStrips(
        jpov_skylight::WaterQuadSideM(), jpov_skylight::kWaterQuadTileRows);
    ASSERT_EQ(strips.size(), static_cast<size_t>(jpov_skylight::kWaterQuadTileRows));
    for (const jpov::Strip32& s : strips) {
        // point_count 必须显式填满（默认 0 会让渲染器 CHECK 崩，说明「忘了填」）。
        EXPECT_EQ(s.point_count, static_cast<uint32_t>(jpov::kStrip32SampleCount));
    }
}

TEST(SkylightWaterQuad, AreaIsTenSquareKilometers) {
    const float side = jpov_skylight::WaterQuadSideM();
    // 边长 × 边长 == 10 km²（1e7 m²）。容差 5 m²（= 0.5 ppm）：这是在检「面积算对了」，
    // 不是在检 float 尾数 —— 实测偏差 0.46 m²，把 side 写成 3000 这种错会红得很难看。
    EXPECT_NEAR(static_cast<double>(side) * static_cast<double>(side), 1.0e7, 5.0);
    EXPECT_NEAR(side, 3162.2777f, 1.0e-2f);
}

TEST(SkylightWaterQuad, AdjacentRowsShareEdgeAndFullSide) {
    const float side = jpov_skylight::WaterQuadSideM();
    const std::vector<jpov::Strip32> strips = jpov_skylight::BuildWaterQuadStrips(
        side, jpov_skylight::kWaterQuadTileRows);
    const float half = 0.5f * side;

    // 每行：沿 X 从 -half 到 +half、y 恒为水面高度、行宽 == side/rows。
    for (const jpov::Strip32& s : strips) {
        EXPECT_NEAR(s.right[0].x(), -half, kEpsPos);
        EXPECT_NEAR(s.left[jpov::kStrip32SampleCount - 1].x(), half, kEpsPos);
        for (int i = 0; i < jpov::kStrip32SampleCount; ++i) {
            EXPECT_NEAR(s.right[i].y(), jpov_skylight::kWaterPlaneY, kEpsPos);
            EXPECT_NEAR(s.left[i].y(), jpov_skylight::kWaterPlaneY, kEpsPos);
            // left 轨在更高的 z 侧（v = 1），right 轨在低 z 侧（v = 0）。
            EXPECT_GT(s.left[i].z(), s.right[i].z());
        }
    }

    // 相邻两行共用一条边：行 r 的 left == 行 r+1 的 right。
    // 这条断言就是「没有缝、没有重叠」的全部内容（差一点点都会在画面上露成一条亮/暗线）。
    for (size_t r = 0; r + 1 < strips.size(); ++r) {
        for (int i = 0; i < jpov::kStrip32SampleCount; ++i) {
            EXPECT_NEAR(strips[r].left[i].z(), strips[r + 1].right[i].z(), kEpsPos);
            EXPECT_NEAR(strips[r].left[i].x(), strips[r + 1].right[i].x(), kEpsPos);
        }
    }

    // 首行贴着 -half、末行贴着 +half：拼起来正好盖满整块面。
    EXPECT_NEAR(strips.front().right[0].z(), -half, kEpsPos);
    EXPECT_NEAR(strips.back().left[0].z(), half, kEpsPos);
}

TEST(SkylightWaterQuad, BatchIsGlassCoverage) {
    const jpov::TranslucentStrip32Batch batch = jpov_skylight::MakeWaterQuadBatch();
    EXPECT_EQ(batch.strips.size(), static_cast<size_t>(jpov_skylight::kWaterQuadTileRows));
    // 玻璃 = 吸收/遮挡介质。设成 kEmissive 会变成「浮在前面发光的一层膜」，是错的。
    EXPECT_EQ(batch.blend, jpov::StripBlend::kCoverage);
    // 平面没有曲率，用折线（kSmooth 要多一张控制点纹理，收益为零）。
    EXPECT_EQ(batch.geometry, jpov::StripInterpolation::kPolyline);
    // 边反必须开着 —— 水面「远处收敛成一条亮带」全靠它。
    ASSERT_TRUE(batch.fresnel.has_value());
    EXPECT_NEAR(batch.fresnel->f0, 0.02f, 1.0e-6f);   // 水的正入射反射率
    EXPECT_NEAR(batch.fresnel->graze_alpha, 1.0f, 1.0e-6f);

    // 逐点颜色 = 统一的水体色（正对视线时 α≈0.1 ⇒ 看穿）。
    const jpov::Color body = jpov_skylight::WaterGlassBodyColor();
    EXPECT_LT(body.a, 0.5f) << "玻璃本体不应是半透之外的实心";
    for (const jpov::Strip32& s : batch.strips) {
        for (const jpov::Color& c : s.point_color) {
            EXPECT_FLOAT_EQ(c.r, body.r);
            EXPECT_FLOAT_EQ(c.g, body.g);
            EXPECT_FLOAT_EQ(c.b, body.b);
            EXPECT_FLOAT_EQ(c.a, body.a);
        }
    }
}

}  // namespace

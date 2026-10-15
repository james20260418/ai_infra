// 植被风顶点标注派生单测 —— GL-free、纯 CPU。
//
// 覆盖 src/wind_flex.h：WindFlexParamsFromBounds / HashPhase01 / DeriveWindFlex /
// 与 mesh.h 的 kWind 校验（含互斥与声明-数据一致性）。

#include "tools/jpov/src/wind_flex.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

// 造一份竖直「树干」mesh：x=z=0，y ∈ [0, height]。
jpov::MeshData MakeTrunk(int n, float height) {
    jpov::MeshData m;
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
    for (int i = 0; i < n; ++i) {
        const float y = height * static_cast<float>(i) / static_cast<float>(n - 1);
        m.positions.push_back(jpov::Vec3f(0.0f, y, 0.0f));
        m.normals.push_back(jpov::Vec3f(0.0f, 1.0f, 0.0f));
    }
    return m;
}

}  // namespace

TEST(WindFlex, ParamsFromBoundsBaseAndScale) {
    const jpov::WindFlexParams p = jpov::WindFlexParamsFromBounds(
        jpov::Vec3f(-2.0f, 1.0f, -1.0f), jpov::Vec3f(2.0f, 5.0f, 3.0f));
    // base = 底面中心 (x 中心 0, 底 y=1, z 中心 1)
    EXPECT_NEAR(p.base.x(), 0.0f, 1e-6f);
    EXPECT_NEAR(p.base.y(), 1.0f, 1e-6f);
    EXPECT_NEAR(p.base.z(), 1.0f, 1e-6f);
    // scale = max(4, 4, 4) = 4
    EXPECT_NEAR(p.scale, 4.0f, 1e-6f);
}

TEST(WindFlex, ParamsScaleIsMaxExtent) {
    const jpov::WindFlexParams p = jpov::WindFlexParamsFromBounds(
        jpov::Vec3f(0.0f, 0.0f, 0.0f), jpov::Vec3f(1.0f, 7.0f, 2.0f));
    EXPECT_NEAR(p.scale, 7.0f, 1e-6f);
}

TEST(WindFlex, PhaseDeterministicAndInRange) {
    // 同坐标恒同值；范围 [0,1)。
    const float a = jpov::HashPhase01(1.25f, -3.5f, 7.0f);
    const float b = jpov::HashPhase01(1.25f, -3.5f, 7.0f);
    EXPECT_FLOAT_EQ(a, b);
    EXPECT_GE(a, 0.0f);
    EXPECT_LT(a, 1.0f);
    // 不同坐标大多不同（避免恒等 / 退化哈希）。
    int distinct = 0;
    for (int i = 0; i < 64; ++i) {
        const float h = jpov::HashPhase01(static_cast<float>(i) * 0.37f, 0.0f, 0.0f);
        if (std::abs(h - a) > 1e-4f) {
            ++distinct;
        }
    }
    EXPECT_GT(distinct, 48);
}

TEST(WindFlex, DeriveBendGrowsWithHeightAndClamps) {
    jpov::MeshData m = MakeTrunk(5, 4.0f);   // 顶层 y=4
    // base 取 (0, 0, 0)，scale=2 → 顶部距离 4/2=2 → clamp 到 1；中段单调增。
    const jpov::WindFlexParams p = jpov::WindFlexParamsFromBounds(
        jpov::Vec3f(0.0f, 0.0f, 0.0f), jpov::Vec3f(0.0f, 4.0f, 0.0f));   // scale=4
    jpov::DeriveWindFlex(p, &m);

    ASSERT_TRUE(jpov::MeshHasFlag(m.flags, jpov::MeshVertexFlags::kWind));
    ASSERT_EQ(m.wind_flex.size(), m.positions.size());
    EXPECT_NEAR(m.wind_flex.front().x(), 0.0f, 1e-6f);   // 基部 = 0
    EXPECT_NEAR(m.wind_flex.back().x(), 1.0f, 1e-6f);    // 顶部距离=scale → 1
    for (size_t i = 1; i < m.wind_flex.size(); ++i) {
        EXPECT_GE(m.wind_flex[i].x(), m.wind_flex[i - 1].x()) << "bend 应单调不减";
        EXPECT_GE(m.wind_flex[i].x(), 0.0f);
        EXPECT_LE(m.wind_flex[i].x(), 1.0f);
        EXPECT_GE(m.wind_flex[i].y(), 0.0f);
        EXPECT_LT(m.wind_flex[i].y(), 1.0f);
    }
}

TEST(WindFlex, DeriveRejectsRelaxMesh) {
    jpov::MeshData m = MakeTrunk(3, 1.0f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kRelax));
    m.relaxations.assign(m.positions.size(), jpov::Vec2f(0.0f, 0.0f));
    const jpov::WindFlexParams p =
        jpov::WindFlexParamsFromBounds(jpov::Vec3f(0, 0, 0), jpov::Vec3f(0, 1, 0));
    EXPECT_DEATH(jpov::DeriveWindFlex(p, &m), "kRelax");
}

TEST(WindFlex, ValidateRejectsWindFlagWithoutData) {
    jpov::MeshData m = MakeTrunk(3, 1.0f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));
    EXPECT_DEATH(m.Validate(), "kWind");
}

TEST(WindFlex, ValidateRejectsWindDataWithoutFlag) {
    jpov::MeshData m = MakeTrunk(3, 1.0f);
    m.wind_flex.assign(m.positions.size(), jpov::Vec2f(0.0f, 0.0f));
    EXPECT_DEATH(m.Validate(), "kWind");
}

TEST(WindFlex, ValidateRejectsRelaxAndWindTogether) {
    jpov::MeshData m = MakeTrunk(3, 1.0f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kRelax) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));
    m.relaxations.assign(m.positions.size(), jpov::Vec2f(0.0f, 0.0f));
    m.wind_flex.assign(m.positions.size(), jpov::Vec2f(0.0f, 0.0f));
    EXPECT_DEATH(m.Validate(), "互斥");
}

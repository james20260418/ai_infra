// FireFogLower 单测 — GL-free CPU 工具：命令层点雾 → 团（FogBody）+ tile 剪枝表。
//
// 覆盖：LowerPointFog 字段映射；BuildTileFogIndexData 的空输入 / 居中投影覆盖 /
// 每 tile cap（先到先得）/ 打包布局 / 近平面后保守全屏。

#include "tools/jpov/src/fire_fog/fire_fog_lower.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace jpov {
namespace {

// 单位正交投影（列主序恒等）：clip = point，w = 1 ⇒ NDC = point ⇒
// 屏幕 = (point*0.5 + 0.5) * viewport。便于精确推 tile 覆盖。
void IdentityMvp(float mvp[16]) {
    for (int i = 0; i < 16; ++i) {
        mvp[i] = 0.0f;
    }
    mvp[0] = 1.0f;
    mvp[5] = 1.0f;
    mvp[10] = 1.0f;
    mvp[15] = 1.0f;
}

// 从打包数据读某 tile 的索引列表（已剔除 sentinel）。
std::vector<int> TileIndices(const std::vector<uint8_t>& packed, int grid_w,
                             int texels_per_tile, int sentinel,
                             int tc, int tr) {
    const int tex_w = grid_w * texels_per_tile;
    std::vector<int> out;
    for (int k = 0; k < texels_per_tile; ++k) {
        const int gx = tc * texels_per_tile + k;
        const uint8_t* px = &packed[(static_cast<size_t>(tr) * tex_w + gx) * 4];
        for (int c = 0; c < 4; ++c) {
            if (px[c] != sentinel) {
                out.push_back(px[c]);
            }
        }
    }
    return out;
}

TEST(FireFogLower, LowerPointFogMapsFields) {
    PointFog fog;
    fog.center = Vec3f(1.0f, 2.0f, 3.0f);
    fog.radius = 2.5f;
    fog.color = Color{0.1f, 0.2f, 0.3f, 1.0f};
    fog.intensity = 0.75f;
    fog.attenuation = FogAttenuation::kQuadratic;

    const FogBody b = LowerPointFog(fog);
    EXPECT_EQ(b.kind, FogFieldKind::kAnalyticProfile);
    EXPECT_FLOAT_EQ(b.bound.center.x(), 1.0f);
    EXPECT_FLOAT_EQ(b.bound.center.y(), 2.0f);
    EXPECT_FLOAT_EQ(b.bound.center.z(), 3.0f);
    // 立方 OBB：各半轴 = 半径；三轴为单位轴。
    EXPECT_FLOAT_EQ(b.bound.half_extent.x(), 2.5f);
    EXPECT_FLOAT_EQ(b.bound.half_extent.y(), 2.5f);
    EXPECT_FLOAT_EQ(b.bound.half_extent.z(), 2.5f);
    EXPECT_FLOAT_EQ(b.bound.axis[0].x(), 1.0f);
    EXPECT_FLOAT_EQ(b.bound.axis[1].y(), 1.0f);
    EXPECT_FLOAT_EQ(b.bound.axis[2].z(), 1.0f);
    EXPECT_FLOAT_EQ(b.color.r, 0.1f);
    EXPECT_FLOAT_EQ(b.color.g, 0.2f);
    EXPECT_FLOAT_EQ(b.color.b, 0.3f);
    EXPECT_FLOAT_EQ(b.intensity, 0.75f);
    // attenuation 落在 params[0]（shader 读 t2.x）。
    EXPECT_FLOAT_EQ(b.params[0], static_cast<float>(FogAttenuation::kQuadratic));
}

TEST(FireFogLower, EmptyBodiesAllSentinel) {
    float mvp[16];
    IdentityMvp(mvp);
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;
    constexpr int kMaxPer = 8;
    constexpr int kTexels = 2;
    constexpr uint8_t kSentinel = 255;
    const std::vector<uint8_t> packed = BuildTileFogIndexData(
        {}, mvp, kGridW, kGridH, kTile, kMaxPer, kTexels, kSentinel);
    const size_t expected = static_cast<size_t>(kGridW * kTexels) * kGridH * 4;
    ASSERT_EQ(packed.size(), expected);
    for (uint8_t v : packed) {
        EXPECT_EQ(v, kSentinel);
    }
}

TEST(FireFogLower, CenteredBodyCoversExpectedTiles) {
    float mvp[16];
    IdentityMvp(mvp);
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;
    constexpr int kMaxPer = 8;
    constexpr int kTexels = 2;
    constexpr uint8_t kSentinel = 255;

    // viewport = 64×64。球心在原点、半径 0.15 ⇒ NDC ±0.15 ⇒ 屏幕 [22.4, 41.6]
    // ⇒ 落在 tile 列/行 1..2（tile1=[16,32)、tile2=[32,48)），避开边界。
    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 0.0f);
    fog.radius = 0.15f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies{LowerPointFog(fog)};

    const std::vector<uint8_t> packed = BuildTileFogIndexData(
        bodies, mvp, kGridW, kGridH, kTile, kMaxPer, kTexels, kSentinel);

    int covered = 0;
    for (int tr = 0; tr < kGridH; ++tr) {
        for (int tc = 0; tc < kGridW; ++tc) {
            const std::vector<int> idx = TileIndices(packed, kGridW, kTexels,
                                                     kSentinel, tc, tr);
            const bool in_region = (tc >= 1 && tc <= 2 && tr >= 1 && tr <= 2);
            if (in_region) {
                ASSERT_EQ(idx.size(), 1u) << "tile(" << tc << "," << tr << ")";
                EXPECT_EQ(idx[0], 0);
                ++covered;
            } else {
                EXPECT_TRUE(idx.empty()) << "tile(" << tc << "," << tr << ")";
            }
        }
    }
    EXPECT_EQ(covered, 4);
}

TEST(FireFogLower, PerTileCapFirstComeFirstServed) {
    float mvp[16];
    IdentityMvp(mvp);
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;
    constexpr int kMaxPer = 8;
    constexpr int kTexels = 2;
    constexpr uint8_t kSentinel = 255;

    // 10 个团全部覆盖同一区域（同中心/半径）⇒ 每 tile 应只记前 8 个（0..7）。
    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 0.0f);
    fog.radius = 0.15f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies;
    for (int i = 0; i < 10; ++i) {
        bodies.push_back(LowerPointFog(fog));
    }
    const std::vector<uint8_t> packed = BuildTileFogIndexData(
        bodies, mvp, kGridW, kGridH, kTile, kMaxPer, kTexels, kSentinel);

    for (int tr = 1; tr <= 2; ++tr) {
        for (int tc = 1; tc <= 2; ++tc) {
            const std::vector<int> idx = TileIndices(packed, kGridW, kTexels,
                                                     kSentinel, tc, tr);
            ASSERT_EQ(idx.size(), static_cast<size_t>(kMaxPer));
            for (int i = 0; i < kMaxPer; ++i) {
                EXPECT_EQ(idx[i], i);   // 先到先得：0,1,...,7
            }
        }
    }
}

TEST(FireFogLower, BehindCameraCoversWholeScreen) {
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;
    constexpr int kMaxPer = 8;
    constexpr int kTexels = 2;
    constexpr uint8_t kSentinel = 255;

    // mvp 令 w = -z ⇒ 在 z>0 处的点 w<0（近平面后）⇒ 保守覆盖全屏。
    float mvp[16];
    for (int i = 0; i < 16; ++i) {
        mvp[i] = 0.0f;
    }
    mvp[0] = 1.0f;
    mvp[5] = 1.0f;
    mvp[11] = -1.0f;   // clip.w = -z
    mvp[15] = 0.0f;

    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 2.0f);   // z>0 ⇒ w<0
    fog.radius = 0.1f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies{LowerPointFog(fog)};

    const std::vector<uint8_t> packed = BuildTileFogIndexData(
        bodies, mvp, kGridW, kGridH, kTile, kMaxPer, kTexels, kSentinel);

    for (int tr = 0; tr < kGridH; ++tr) {
        for (int tc = 0; tc < kGridW; ++tc) {
            const std::vector<int> idx = TileIndices(packed, kGridW, kTexels,
                                                     kSentinel, tc, tr);
            ASSERT_EQ(idx.size(), 1u) << "tile(" << tc << "," << tr << ")";
            EXPECT_EQ(idx[0], 0);
        }
    }
}

TEST(FireFogLower, ZRangeEmptyBodiesAllZero) {
    float mvp[16];
    IdentityMvp(mvp);
    const std::vector<float> zr =
        BuildTileZRangeData({}, mvp, Vec3f(0.0f, 0.0f, -10.0f), 4, 4, 16, 0.1f, 2000.0f);
    ASSERT_EQ(zr.size(), static_cast<size_t>(4 * 4 * 2));
    for (float v : zr) {
        EXPECT_EQ(v, 0.0f);   // 无候选 ⇒ (0,0)
    }
}

TEST(FireFogLower, ZRangeCenteredBodyCoversExpectedTiles) {
    float mvp[16];
    IdentityMvp(mvp);
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;

    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 0.0f);
    fog.radius = 0.15f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies{LowerPointFog(fog)};

    // 相机在 (0,0,-10)：团心距相机 d=10，外接球 r=radius*√3（立方 OBB）。
    const std::vector<float> zr = BuildTileZRangeData(
        bodies, mvp, Vec3f(0.0f, 0.0f, -10.0f), kGridW, kGridH, kTile, 0.1f, 2000.0f);
    const float r = 0.15f * std::sqrt(3.0f);
    const float zmn = 10.0f - r;
    const float zmx = 10.0f + r;

    int hit = 0;
    for (int tr = 0; tr < kGridH; ++tr) {
        for (int tc = 0; tc < kGridW; ++tc) {
            const float a = zr[static_cast<size_t>((tr * kGridW + tc) * 2 + 0)];
            const float b = zr[static_cast<size_t>((tr * kGridW + tc) * 2 + 1)];
            const bool in_region = (tc >= 1 && tc <= 2 && tr >= 1 && tr <= 2);
            if (in_region) {
                EXPECT_NEAR(a, zmn, 1e-3f) << "tile(" << tc << "," << tr << ")";
                EXPECT_NEAR(b, zmx, 1e-3f) << "tile(" << tc << "," << tr << ")";
                ++hit;
            } else {
                EXPECT_EQ(a, 0.0f) << "tile(" << tc << "," << tr << ")";
                EXPECT_EQ(b, 0.0f) << "tile(" << tc << "," << tr << ")";
            }
        }
    }
    EXPECT_EQ(hit, 4);
}

TEST(FireFogLower, ZRangeBehindCameraCoversWholeScreen) {
    constexpr int kGridW = 4;
    constexpr int kGridH = 4;
    constexpr int kTile = 16;

    float mvp[16];
    for (int i = 0; i < 16; ++i) {
        mvp[i] = 0.0f;
    }
    mvp[0] = 1.0f;
    mvp[5] = 1.0f;
    mvp[11] = -1.0f;   // clip.w = -z ⇒ z>0 处 w<0（近平面后）
    mvp[15] = 0.0f;

    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 2.0f);
    fog.radius = 0.1f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies{LowerPointFog(fog)};

    const std::vector<float> zr =
        BuildTileZRangeData(bodies, mvp, Vec3f(0.0f, 0.0f, 0.0f), kGridW, kGridH, kTile,
                            0.1f, 2000.0f);
    const float r = 0.1f * std::sqrt(3.0f);   // d=2（团心到原点相机）
    const float zmn = 2.0f - r;
    const float zmx = 2.0f + r;
    for (int t = 0; t < kGridW * kGridH; ++t) {
        EXPECT_NEAR(zr[static_cast<size_t>(t * 2 + 0)], zmn, 1e-3f);
        EXPECT_NEAR(zr[static_cast<size_t>(t * 2 + 1)], zmx, 1e-3f);
    }
}

TEST(FireFogLower, ZRangeBodyOutsideDepthRangeAllZero) {
    float mvp[16];
    IdentityMvp(mvp);

    // 团心距相机 3000m，z_far=2000 ⇒ 完全在 froxel z 区间外 ⇒ 无 tile 命中。
    PointFog fog;
    fog.center = Vec3f(0.0f, 0.0f, 3000.0f);
    fog.radius = 0.15f;
    fog.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    fog.intensity = 1.0f;
    fog.attenuation = FogAttenuation::kUniform;
    std::vector<FogBody> bodies{LowerPointFog(fog)};

    const std::vector<float> zr =
        BuildTileZRangeData(bodies, mvp, Vec3f(0.0f, 0.0f, 0.0f), 4, 4, 16, 0.1f, 2000.0f);
    for (float v : zr) {
        EXPECT_EQ(v, 0.0f);
    }
}

}  // namespace
}  // namespace jpov

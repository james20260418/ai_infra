// JPOV 局部体积雾 — CPU tile culling 单测（纯 CPU，GL-free）

#include <gtest/gtest.h>

#include "tools/jpov/src/volumetric_fog/fog_common.h"
#include "tools/jpov/src/volumetric_fog/tile_fog_culling.h"

namespace jpov {
namespace volumetric_fog {
namespace {

// 正交式 MVP：clip = (x, y, 0, 1) ⇒ NDC = (x, y)。世界原点落在屏幕中心。
void OrthoMvp(float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[15] = 1.0f;
}

// 读某 tile 的 index 列表（返回出现的 index 集合，遇到哨兵停）。
std::vector<uint16_t> TileList(const std::vector<uint16_t>& idx,
                               const FogTileLayout& l, int tx, int ty) {
    std::vector<uint16_t> out;
    for (int i = 0; i < kMaxFogPerTile; ++i) {
        const uint16_t v =
            idx[static_cast<size_t>(ty) * l.tex_w + tx * kMaxFogPerTile + i];
        if (v == kFogIndexSentinel) break;
        out.push_back(v);
    }
    return out;
}

TEST(TileFogCullingTest, CenterBlobHitsCenterTile) {
    float mvp[16];
    OrthoMvp(mvp);
    const int W = 64, H = 64;
    const FogTileLayout l = ComputeFogTileLayout(W, H, kFogTileSize, kMaxFogPerTile);
    EXPECT_EQ(l.grid_w, 4);
    EXPECT_EQ(l.grid_h, 4);

    FogSphere f;
    f.center = {0.0f, 0.0f, 0.0f};
    f.radius = 0.3f;   // NDC 半径 0.3 ⇒ 屏幕半径 ~0.15*W ≈ 10px ⇒ 只覆盖中心 tile
    std::vector<FogSphere> spheres = {f};
    std::vector<uint16_t> idx(static_cast<size_t>(l.tex_w) * l.tex_h);
    BuildFogTileIndices(spheres, {}, W, H, mvp, kFogTileSize, kMaxFogPerTile, l, &idx);

    // 中心 tile (2,2)（NDC 原点 = 中心）。
    const auto center = TileList(idx, l, 2, 2);
    ASSERT_EQ(center.size(), 1u);
    EXPECT_EQ(center[0], 0);
    // 角落 tile 不应包含。
    EXPECT_TRUE(TileList(idx, l, 0, 0).empty());
}

TEST(TileFogCullingTest, CapTruncatesToK) {
    float mvp[16];
    OrthoMvp(mvp);
    const int W = 32, H = 32;
    const FogTileLayout l = ComputeFogTileLayout(W, H, kFogTileSize, kMaxFogPerTile);
    // 造 kMaxFogPerTile + 5 个都覆盖整个屏幕的雾体 → 每个 tile 只能留 K 个（先到先得）。
    std::vector<FogSphere> spheres;
    for (int i = 0; i < kMaxFogPerTile + 5; ++i) {
        FogSphere f;
        f.center = {0.0f, 0.0f, 0.0f};
        f.radius = 100.0f;   // 远大于屏 ⇒ 覆盖全屏
        spheres.push_back(f);
    }
    std::vector<uint16_t> idx(static_cast<size_t>(l.tex_w) * l.tex_h);
    BuildFogTileIndices(spheres, {}, W, H, mvp, kFogTileSize, kMaxFogPerTile, l, &idx);
    const auto list = TileList(idx, l, 0, 0);
    EXPECT_EQ(static_cast<int>(list.size()), kMaxFogPerTile);   // 恰好 K
    // 先到先得 ⇒ 保留下标 0..K-1。
    for (int i = 0; i < kMaxFogPerTile; ++i) {
        EXPECT_EQ(list[i], i);
    }
}

TEST(TileFogCullingTest, OffscreenBlobWritesNothing) {
    float mvp[16];
    OrthoMvp(mvp);
    const int W = 32, H = 32;
    const FogTileLayout l = ComputeFogTileLayout(W, H, kFogTileSize, kMaxFogPerTile);
    FogSphere f;
    f.center = {50.0f, 50.0f, 0.0f};   // NDC 远超 [-1,1]
    f.radius = 0.1f;
    std::vector<FogSphere> spheres = {f};
    std::vector<uint16_t> idx(static_cast<size_t>(l.tex_w) * l.tex_h);
    BuildFogTileIndices(spheres, {}, W, H, mvp, kFogTileSize, kMaxFogPerTile, l, &idx);
    for (int ty = 0; ty < l.grid_h; ++ty) {
        for (int tx = 0; tx < l.grid_w; ++tx) {
            EXPECT_TRUE(TileList(idx, l, tx, ty).empty());
        }
    }
}

TEST(TileFogCullingTest, CylinderIndexFollowsSphereOrdering) {
    float mvp[16];
    OrthoMvp(mvp);
    const int W = 32, H = 32;
    const FogTileLayout l = ComputeFogTileLayout(W, H, kFogTileSize, kMaxFogPerTile);
    FogSphere s;
    s.center = {0, 0, 0};
    s.radius = 100.0f;
    FogCylinder c;
    c.base = {0, 0, 0};
    c.axis = {0, 1, 0};
    c.radius = 100.0f;
    c.height = 100.0f;
    std::vector<FogSphere> spheres = {s};
    std::vector<FogCylinder> cyls = {c};
    std::vector<uint16_t> idx(static_cast<size_t>(l.tex_w) * l.tex_h);
    BuildFogTileIndices(spheres, cyls, W, H, mvp, kFogTileSize, kMaxFogPerTile, l,
                        &idx);
    const auto list = TileList(idx, l, 1, 1);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], 0);   // 球在前
    EXPECT_EQ(list[1], 1);   // 圆柱在后
}

}  // namespace
}  // namespace volumetric_fog
}  // namespace jpov

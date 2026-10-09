// JPOV Fire-Fog — GL-free CPU 工具：命令层点雾 → 后端团（FogBody）+ 屏幕 tile 剪枝表
//
// 设计见 tools/jpov/docs/jpov_froxel_design.md。本文件只做**纯 CPU** 的两件事，
// 与 GL 无关、可单测：
//   1. LowerPointFog：把命令层 PointFog（球）降维成后端统一团 FogBody（立方 OBB）；
//   2. BuildTileFogIndexData：把团列表投影成「每 tile ≤K 个团索引」的 tile 索引纹理数据
//      （RGBA8；每 tile kTexelsPerTile 个 texel，每 texel RGBA 各 1 个 uint8 团索引）。
//   3. BuildTileZRangeData：每 tile 的**保守 z 范围**，供 inject 短路「该切片是否可能碰雾」。
//
// MVP 约定：只有 kAnalyticProfile；PointFog.intensity 直接作为消光尺度（σ = intensity·profile），
// 光照明用常量发射色（color）。这些语义在 fire_fog_shader.h 里与之对应。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/interface/render_command.h"  // PointFog / Vec3f
#include "tools/jpov/src/fire_fog/convex_hull_tiles.h"
#include "tools/jpov/src/fire_fog/fog_body.h"

namespace jpov {

// 命令层点雾 → 后端统一团。
//
// 立方 OBB：中心 = fog.center，三轴 = 世界单位轴，三半轴长 = fog.radius（故 box ⊇ 球，
// 剪枝保守不漏）。kind = kAnalyticProfile；params[0] = attenuation（剖面枚举）。
// MVP 光照：不采样任何光源，L_in = 常量 = color。sigma_scale 预留未用（置 1）。
// Pre-condition: fog.radius > 0
inline FogBody LowerPointFog(const PointFog& fog) {
    CHECK_GT(fog.radius, 0.0f) << "PointFog.radius 必须 > 0";
    FogBody b;
    b.bound.center = fog.center;
    b.bound.axis[0] = Vec3f(1.0f, 0.0f, 0.0f);
    b.bound.axis[1] = Vec3f(0.0f, 1.0f, 0.0f);
    b.bound.axis[2] = Vec3f(0.0f, 0.0f, 1.0f);
    b.bound.half_extent = Vec3f(fog.radius, fog.radius, fog.radius);
    b.kind = FogFieldKind::kAnalyticProfile;
    b.color = fog.color;
    b.intensity = fog.intensity;
    b.sigma_scale = 1.0f;   // MVP 未用（σ 由 intensity 直接给出）
    for (int i = 0; i < 8; ++i) {
        b.params[i] = 0.0f;
    }
    b.params[0] = static_cast<float>(fog.attenuation);
    b.prebaked_lin = Vec3f(0.0f, 0.0f, 0.0f);
    return b;
}

// 某团 OBB 覆盖的屏幕 tile（凸包）。近平面后（任一角 w≤0）⇒ *out_behind=true 且 out_tiles 清空，
// 调用方按「保守全屏」处理。
//
// 步骤：8 角点投影到屏幕像素 → 屏幕凸包（convex_hull_tiles.h）→ 覆盖的 tile。
// Pre-conditions: grid_w >= 1, grid_h >= 1, tile_size > 0
inline void AppendBodyCoveredTiles(const FogBody& b,
                                   const float mvp[16],
                                   int tile_size,
                                   int grid_w,
                                   int grid_h,
                                   std::vector<TileCoord>* out_tiles /*output*/,
                                   bool* out_behind /*output*/) {
    CHECK(out_tiles != nullptr);
    CHECK(out_behind != nullptr);
    CHECK_GE(grid_w, 1);
    CHECK_GE(grid_h, 1);
    CHECK_GT(tile_size, 0);
    out_tiles->clear();
    *out_behind = false;

    const float fw = static_cast<float>(grid_w * tile_size);
    const float fh = static_cast<float>(grid_h * tile_size);
    const auto project = [&](const Vec3f& p, float* px /*output*/, float* py /*output*/,
                             float* w /*output*/) {
        const float cx = mvp[0] * p.x() + mvp[4] * p.y() + mvp[8] * p.z() + mvp[12];
        const float cy = mvp[1] * p.x() + mvp[5] * p.y() + mvp[9] * p.z() + mvp[13];
        const float cw = mvp[3] * p.x() + mvp[7] * p.y() + mvp[11] * p.z() + mvp[15];
        *w = cw;
        if (cw <= 0.0f) {
            *px = 0.0f;
            *py = 0.0f;
            return;
        }
        const float iw = 1.0f / cw;
        *px = (cx * iw * 0.5f + 0.5f) * fw;
        *py = (cy * iw * 0.5f + 0.5f) * fh;
    };

    // 8 个 OBB 角点：center ± Σ_i axis[i]·half_extent[i]。
    std::vector<Vec2d> pts;
    pts.reserve(8);
    for (int s = 0; s < 8; ++s) {
        const float sx = (s & 1) ? 1.0f : -1.0f;
        const float sy = (s & 2) ? 1.0f : -1.0f;
        const float sz = (s & 4) ? 1.0f : -1.0f;
        Vec3f corner = b.bound.center;
        for (int a = 0; a < 3; ++a) {
            const float sign = (a == 0) ? sx : ((a == 1) ? sy : sz);
            corner = corner + b.bound.axis[a] * (b.bound.half_extent[a] * sign);
        }
        float px = 0.0f;
        float py = 0.0f;
        float w = 0.0f;
        project(corner, &px, &py, &w);
        if (w <= 0.0f) {
            *out_behind = true;
            return;
        }
        pts.push_back(Vec2d(static_cast<double>(px), static_cast<double>(py)));
    }

    const TileGrid grid{static_cast<double>(tile_size), grid_w, grid_h};
    AppendConvexHullCoveredTiles(grid, pts, out_tiles);
}

// 由团列表 + 相机 MVP 生成 tile 索引纹理（RGBA8）的像素数据。
//
// 步骤：每团取 OBB 8 角点 → 投影到屏幕像素 → 屏幕凸包（convex_hull_tiles.h）→ 覆盖的
// tile；按列表顺序“先到先得”，每 tile 超过 max_per_tile 则丢弃多余团。任一角在近平面后
//（w ≤ 0）→ 该团保守覆盖全屏（与点光源剪枝同规则）。
//
// 输出布局：宽 = grid_w*texels_per_tile，高 = grid_h；每 tile 的 texels_per_tile 个 texel
// 依次接排；每 texel 的 RGBA 各放一个 uint8 团索引；空槽填 sentinel。
// 返回的 vector 可直接 glTexImage2D(..., GL_RGBA, GL_UNSIGNED_BYTE, data)。
//
// Pre-conditions: grid_w >= 1, grid_h >= 1, tile_size > 0, max_per_tile == texels_per_tile*4
inline std::vector<uint8_t> BuildTileFogIndexData(const std::vector<FogBody>& bodies,
                                                  const float mvp[16],
                                                  int grid_w,
                                                  int grid_h,
                                                  int tile_size,
                                                  int max_per_tile,
                                                  int texels_per_tile,
                                                  uint8_t sentinel) {
    CHECK_GE(grid_w, 1);
    CHECK_GE(grid_h, 1);
    CHECK_GT(tile_size, 0);
    CHECK_EQ(max_per_tile, texels_per_tile * 4);

    const int tex_w = grid_w * texels_per_tile;
    const int total_tiles = grid_w * grid_h;
    std::vector<uint8_t> counts(static_cast<size_t>(total_tiles), 0);
    std::vector<uint8_t> idx(static_cast<size_t>(total_tiles) * max_per_tile, sentinel);

    const int body_count = static_cast<int>(bodies.size());
    for (int bi = 0; bi < body_count; ++bi) {
        std::vector<TileCoord> covered;
        bool behind = false;
        AppendBodyCoveredTiles(bodies[bi], mvp, tile_size, grid_w, grid_h, &covered, &behind);

        if (behind) {
            // 近平面后 → 保守覆盖全屏。
            for (int t = 0; t < total_tiles; ++t) {
                uint8_t& cnt = counts[t];
                if (cnt >= static_cast<uint8_t>(max_per_tile)) {
                    continue;
                }
                idx[static_cast<size_t>(t) * max_per_tile + cnt] = static_cast<uint8_t>(bi);
                ++cnt;
            }
            continue;
        }
        for (const TileCoord& tc : covered) {
            if (tc.x < 0 || tc.x >= grid_w || tc.y < 0 || tc.y >= grid_h) {
                continue;
            }
            const int t = tc.y * grid_w + tc.x;
            uint8_t& cnt = counts[t];
            if (cnt >= static_cast<uint8_t>(max_per_tile)) {
                continue;   // 先到先得
            }
            idx[static_cast<size_t>(t) * max_per_tile + cnt] = static_cast<uint8_t>(bi);
            ++cnt;
        }
    }

    std::vector<uint8_t> packed(static_cast<size_t>(tex_w) * grid_h * 4, sentinel);
    for (int tr = 0; tr < grid_h; ++tr) {
        for (int tc = 0; tc < grid_w; ++tc) {
            const int t = tr * grid_w + tc;
            for (int k = 0; k < texels_per_tile; ++k) {
                const int gx = tc * texels_per_tile + k;
                uint8_t* px = &packed[(static_cast<size_t>(tr) * tex_w + gx) * 4];
                px[0] = idx[static_cast<size_t>(t) * max_per_tile + k * 4 + 0];
                px[1] = idx[static_cast<size_t>(t) * max_per_tile + k * 4 + 1];
                px[2] = idx[static_cast<size_t>(t) * max_per_tile + k * 4 + 2];
                px[3] = idx[static_cast<size_t>(t) * max_per_tile + k * 4 + 3];
            }
        }
    }
    return packed;
}

// 每 tile 的**保守 z 范围**（2 float/tile：zmin, zmax），供 inject 短路。
//
// 团的 z 范围按「团心到相机的距离 d ± 外接球半径」保守估计：
//   [max(z_near, d − r), min(z_far, d + r)]，r = |half_extent|（OBB 外接球）。
// 该区间是团实际相交深度的**超集**（保守）⇒ 短路安全，不会漏掉真正相交的切片。
// 覆盖 tile 与 BuildTileFogIndexData 同源（凸包；近平面后 = 全屏）。
// 空 tile 返回 (0, 0)。
//
// 输出布局：宽 = grid_w，高 = grid_h，逐行接排，每 tile 2 个 float (zmin, zmax)。
// 可直接 glTexImage2D(..., GL_RG, GL_FLOAT, data)。
//
// Pre-conditions: grid_w >= 1, grid_h >= 1, tile_size > 0, z_far > z_near > 0
inline std::vector<float> BuildTileZRangeData(const std::vector<FogBody>& bodies,
                                              const float mvp[16],
                                              const Vec3f& cam_pos,
                                              int grid_w,
                                              int grid_h,
                                              int tile_size,
                                              float z_near,
                                              float z_far) {
    CHECK_GE(grid_w, 1);
    CHECK_GE(grid_h, 1);
    CHECK_GT(tile_size, 0);
    CHECK_GT(z_near, 0.0f);
    CHECK_GT(z_far, z_near);

    const int total_tiles = grid_w * grid_h;
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> zmin(static_cast<size_t>(total_tiles), inf);
    std::vector<float> zmax(static_cast<size_t>(total_tiles), -inf);

    const int body_count = static_cast<int>(bodies.size());
    for (int bi = 0; bi < body_count; ++bi) {
        const FogBody& b = bodies[bi];
        const float dx = b.bound.center.x() - cam_pos.x();
        const float dy = b.bound.center.y() - cam_pos.y();
        const float dz = b.bound.center.z() - cam_pos.z();
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float r = std::sqrt(b.bound.half_extent.x() * b.bound.half_extent.x() +
                                  b.bound.half_extent.y() * b.bound.half_extent.y() +
                                  b.bound.half_extent.z() * b.bound.half_extent.z());
        const float zmn = std::max(z_near, d - r);
        const float zmx = std::min(z_far, d + r);
        if (zmx <= zmn) {
            continue;   // 该团完全在 froxel z 区间之外
        }

        std::vector<TileCoord> covered;
        bool behind = false;
        AppendBodyCoveredTiles(b, mvp, tile_size, grid_w, grid_h, &covered, &behind);

        const auto merge_tile = [&](int t) {
            zmin[static_cast<size_t>(t)] = std::min(zmin[static_cast<size_t>(t)], zmn);
            zmax[static_cast<size_t>(t)] = std::max(zmax[static_cast<size_t>(t)], zmx);
        };
        if (behind) {
            for (int t = 0; t < total_tiles; ++t) {
                merge_tile(t);
            }
            continue;
        }
        for (const TileCoord& tc : covered) {
            if (tc.x < 0 || tc.x >= grid_w || tc.y < 0 || tc.y >= grid_h) {
                continue;
            }
            merge_tile(tc.y * grid_w + tc.x);
        }
    }

    std::vector<float> out(static_cast<size_t>(total_tiles) * 2, 0.0f);
    for (int t = 0; t < total_tiles; ++t) {
        if (zmin[static_cast<size_t>(t)] <= zmax[static_cast<size_t>(t)]) {
            out[static_cast<size_t>(t) * 2 + 0] = zmin[static_cast<size_t>(t)];
            out[static_cast<size_t>(t) * 2 + 1] = zmax[static_cast<size_t>(t)];
        }   // 否则保持 (0,0) = 无候选
    }
    return out;
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_

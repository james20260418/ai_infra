// JPOV Fire-Fog — GL-free CPU 工具：命令层点雾 → 后端团（FogBody）+ 屏幕 tile 剪枝表
//
// 设计见 tools/jpov/docs/jpov_froxel_design.md。本文件只做**纯 CPU** 的两件事，
// 与 GL 无关、可单测：
//   1. LowerPointFog：把命令层 PointFog（球）降维成后端统一团 FogBody（立方 OBB）；
//   2. BuildTileFogIndexData：把团列表投影成「每 tile ≤K 个团索引」的 tile 索引纹理数据
//      （RGBA8；每 tile kTexelsPerTile 个 texel，每 texel RGBA 各 1 个 uint8 团索引）。
//   3. BuildTileZRangeData：每 tile 的**保守 z 范围**，供 inject 短路「该切片是否可能碰雾」。
//
// MVP 约定：只有 kRoundBox（圆角盒，替换早期点雾/球）；PointFog.sigma 为消光系数（点处
// σ = sigma·w，w 由圆角盒距离 + 剖面给出），albedo 为散射色（乘光照），emission 为自发光
// （与密度无关）。这些语义在 fire_fog_shader.h 里与之对应（光照始终开启）。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/interface/render_command.h"  // PointFog / Vec3f
#include "tools/jpov/src/fire_fog/convex_hull_tiles.h"
#include "tools/jpov/src/fire_fog/fog_body.h"

namespace jpov {

// 命令层雾体 → 后端统一团（**kRoundBox** 圆角盒）。
//
// 容器 OBB：中心 = fog.center，三轴 = 世界单位轴，三半轴长 = 内盒半尺寸(size/2) + 外侧圆角壳
// 厚度 radius（= box ⊕ 球 的外沿，剪枝 / z 范围保守不漏）。kind = kRoundBox；
// params[0] = attenuation（外壳剖面枚举）、params[1] = radius（外侧圆角壳厚度）。
// sigma / albedo / emission 原样映射（散射与自发光分开；光照由 inject shader 始终开启）。
// Pre-conditions: fog.size 各分量 > 0；fog.radius >= 0；fog.sigma >= 0；albedo ∈ [0,1]；emission >= 0
inline FogBody LowerPointFog(const PointFog& fog) {
    CHECK_GT(fog.size.x(), 0.0f) << "PointFog.size 各分量必须 > 0";
    CHECK_GT(fog.size.y(), 0.0f) << "PointFog.size 各分量必须 > 0";
    CHECK_GT(fog.size.z(), 0.0f) << "PointFog.size 各分量必须 > 0";
    CHECK_GE(fog.radius, 0.0f) << "PointFog.radius（外侧圆角壳厚度）必须 >= 0";
    CHECK_GE(fog.sigma, 0.0f) << "PointFog.sigma（消光系数 1/m）必须 >= 0";
    CHECK_GE(fog.albedo.r, 0.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(fog.albedo.g, 0.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(fog.albedo.b, 0.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(fog.albedo.r, 1.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(fog.albedo.g, 1.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(fog.albedo.b, 1.0f) << "PointFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(fog.emission.r, 0.0f) << "PointFog.emission 各通道必须 >= 0";
    CHECK_GE(fog.emission.g, 0.0f) << "PointFog.emission 各通道必须 >= 0";
    CHECK_GE(fog.emission.b, 0.0f) << "PointFog.emission 各通道必须 >= 0";

    FogBody b;
    b.bound.center = fog.center;
    b.bound.axis[0] = Vec3f(1.0f, 0.0f, 0.0f);
    b.bound.axis[1] = Vec3f(0.0f, 1.0f, 0.0f);
    b.bound.axis[2] = Vec3f(0.0f, 0.0f, 1.0f);
    // 容器半尺寸 = 内盒半 (size/2) + 外侧圆角壳厚度 radius（culling/z 范围用外沿，保守不漏）。
    b.bound.half_extent = Vec3f(fog.size.x() * 0.5f + fog.radius,
                                fog.size.y() * 0.5f + fog.radius,
                                fog.size.z() * 0.5f + fog.radius);
    b.kind = FogFieldKind::kRoundBox;
    b.sigma = fog.sigma;
    b.albedo = fog.albedo;
    b.emission = fog.emission;
    for (int i = 0; i < 32; ++i) {   // 零掉整个参数区（params 已扩到 32；未用位保持 0）
        b.params[i] = 0.0f;
    }
    b.params[0] = static_cast<float>(fog.attenuation);
    b.params[1] = fog.radius;   // 外侧圆角壳厚度
    return b;
}

// 命令层火焰 → 后端统一团（**kFlame** 燃料盒包裹燃烧）。
//
// 「低频形」= 燃料盒**表面外侧壳**（shell，完整包裹） + **盒沿世界 +Y 挤出**的羽流柱（长 plume；
// 横向随高度微扩 expand、强度按 (1−s)^decay 软顶）；**风**（只取水平分量）把截面顺风平移 lean·s+bend·s²。
// 浮力固定 +Y；无接缝；侧向为“充气轮廓”软边。
// 容器 AABB（CPU 剪枝 / z 范围）= 上式的**保守** world-轴 AABB（燃料盒⊕shell ∪ 羽流柱，含充气与剪切）；
// 容器与**已折算的** lean/bend（由风速推得，见 fog_body.h kFlameWind*）一并**打包**——GLSL 直接
// 消费，不各自持常量 / 不重算公式（单一来源）。燃料盒 center/box_half **直接打包**。
// params 字段 → 见 fog_body.h flame_param。
// Pre-conditions: size 各分量 > 0；up 非零；shell > 0；plume/expand/decay/vigor/sigma >= 0；
//                 albedo ∈ [0,1]；emission >= 0
inline FogBody LowerFlameFog(const FlameFog& f) {
    CHECK_GT(f.size.x(), 0.0f) << "FlameFog.size 各分量必须 > 0";
    CHECK_GT(f.size.y(), 0.0f) << "FlameFog.size 各分量必须 > 0";
    CHECK_GT(f.size.z(), 0.0f) << "FlameFog.size 各分量必须 > 0";
    CHECK_GT(f.shell, 0.0f) << "FlameFog.shell（包裹壳厚）必须 > 0";
    CHECK_GE(f.plume, 0.0f) << "FlameFog.plume（羽流段长）必须 >= 0";
    CHECK_GE(f.expand, 0.0f) << "FlameFog.expand（半径微扩系数）必须 >= 0";
    CHECK_GE(f.decay, 0.0f) << "FlameFog.decay（强度衰减指数）必须 >= 0";
    CHECK_GE(f.vigor, 0.0f) << "FlameFog.vigor（旺盛度）必须 >= 0";
    CHECK_GE(f.sigma, 0.0f) << "FlameFog.sigma（消光系数 1/m）必须 >= 0";
    CHECK_GE(f.albedo.r, 0.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(f.albedo.g, 0.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(f.albedo.b, 0.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(f.albedo.r, 1.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(f.albedo.g, 1.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(f.albedo.b, 1.0f) << "FlameFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(f.emission.r, 0.0f) << "FlameFog.emission 各通道必须 >= 0";
    CHECK_GE(f.emission.g, 0.0f) << "FlameFog.emission 各通道必须 >= 0";
    CHECK_GE(f.emission.b, 0.0f) << "FlameFog.emission 各通道必须 >= 0";

    const Vec3f box_half = f.size * 0.5f;

    // 风（只取【水平】分量）→ 截面平移（已折算）：只有水平单位方向 + 两个标量进 GPU。
    const float wx = f.wind.x();
    const float wz = f.wind.z();
    const float wind_speed = std::sqrt(wx * wx + wz * wz);
    float wind_x = 0.0f;
    float wind_z = 0.0f;
    float lean = 0.0f;
    float bend = 0.0f;
    if (wind_speed > 1.0e-6f) {
        wind_x = wx / wind_speed;
        wind_z = wz / wind_speed;
        lean = kFlameWindLean * wind_speed;
        bend = kFlameWindBend * std::pow(wind_speed, 1.3f);
    }

    // 容器 AABB（保守）：燃料盒 ⊕ shell ∪ 羽流柱（盒顶沿 +Y，含横向充气 delta 与顺风剪切 shear）。
    Vec3f lo = f.center - box_half - Vec3f(f.shell, f.shell, f.shell);
    Vec3f hi = f.center + box_half + Vec3f(f.shell, f.shell, f.shell);
    if (f.plume > 0.0f) {
        const float ref_len = std::min(box_half.x(), std::min(box_half.y(), box_half.z()));
        const float delta = f.expand * ref_len;   // 横向充气上限（s=1）
        const float shear = lean + bend;           // 顺风剪切上限（s=1）
        const float hx = box_half.x() + delta;
        const float hz = box_half.z() + delta;
        const float sx = wind_x * shear;
        const float sz = wind_z * shear;
        lo = Vec3f(std::min(lo.x(), std::min(f.center.x() - hx, f.center.x() + sx - hx)),
                   std::min(lo.y(), f.center.y() + box_half.y()),
                   std::min(lo.z(), std::min(f.center.z() - hz, f.center.z() + sz - hz)));
        hi = Vec3f(std::max(hi.x(), std::max(f.center.x() + hx, f.center.x() + sx + hx)),
                   std::max(hi.y(), f.center.y() + box_half.y() + f.plume),
                   std::max(hi.z(), std::max(f.center.z() + hz, f.center.z() + sz + hz)));
    }

    FogBody b;
    b.kind = FogFieldKind::kFlame;
    b.bound.axis[0] = Vec3f(1.0f, 0.0f, 0.0f);
    b.bound.axis[1] = Vec3f(0.0f, 1.0f, 0.0f);
    b.bound.axis[2] = Vec3f(0.0f, 0.0f, 1.0f);
    b.bound.center = (lo + hi) * 0.5f;
    b.bound.half_extent = (hi - lo) * 0.5f;
    b.sigma = f.sigma;
    b.albedo = f.albedo;
    b.emission = f.emission;
    for (int i = 0; i < 32; ++i) {
        b.params[i] = 0.0f;
    }
    b.params[flame_param::kContX] = b.bound.center.x();
    b.params[flame_param::kContY] = b.bound.center.y();
    b.params[flame_param::kContZ] = b.bound.center.z();
    b.params[flame_param::kContHx] = b.bound.half_extent.x();
    b.params[flame_param::kContHy] = b.bound.half_extent.y();
    b.params[flame_param::kContHz] = b.bound.half_extent.z();
    b.params[flame_param::kCenterX] = f.center.x();
    b.params[flame_param::kCenterY] = f.center.y();
    b.params[flame_param::kCenterZ] = f.center.z();
    b.params[flame_param::kBoxHx] = box_half.x();
    b.params[flame_param::kBoxHy] = box_half.y();
    b.params[flame_param::kBoxHz] = box_half.z();
    b.params[flame_param::kWindX] = wind_x;
    b.params[flame_param::kWindZ] = wind_z;
    b.params[flame_param::kLean] = lean;
    b.params[flame_param::kBend] = bend;
    b.params[flame_param::kShell] = f.shell;
    b.params[flame_param::kPlume] = f.plume;
    b.params[flame_param::kVigor] = f.vigor;
    b.params[flame_param::kExpand] = f.expand;
    b.params[flame_param::kDecay] = f.decay;
    return b;
}

// 把命令层 VolumeFog（共享 3D 火纹理体积）降维成后端统一团 FogBody（kind = kVolumeTex）。
//
// 容器 OBB = 体积盒本身（MVP 轴对齐）；盒直接映射到纹理 [0,1]^3。几何进 params（volume_param
// 下标）；渲染器打包时再写入 union 的 center/bound_h* + density_pow/temp_scale。
inline FogBody LowerVolumeFog(const VolumeFog& v) {
    CHECK_GT(v.size.x(), 0.0f) << "VolumeFog.size 各分量必须 > 0";
    CHECK_GT(v.size.y(), 0.0f) << "VolumeFog.size 各分量必须 > 0";
    CHECK_GT(v.size.z(), 0.0f) << "VolumeFog.size 各分量必须 > 0";
    CHECK_GE(v.sigma, 0.0f) << "VolumeFog.sigma（消光系数 1/m）必须 >= 0";
    CHECK_GE(v.density_pow, 1.0f) << "VolumeFog.density_pow（密度幂）必须 >= 1";
    CHECK_GE(v.temp_scale, 0.0f) << "VolumeFog.temp_scale（温度→自发光放大）必须 >= 0";
    CHECK_GE(v.albedo.r, 0.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(v.albedo.g, 0.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(v.albedo.b, 0.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(v.albedo.r, 1.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(v.albedo.g, 1.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_LE(v.albedo.b, 1.0f) << "VolumeFog.albedo 各通道必须 ∈ [0,1]";
    CHECK_GE(v.emission.r, 0.0f) << "VolumeFog.emission 各通道必须 >= 0";
    CHECK_GE(v.emission.g, 0.0f) << "VolumeFog.emission 各通道必须 >= 0";
    CHECK_GE(v.emission.b, 0.0f) << "VolumeFog.emission 各通道必须 >= 0";

    const Vec3f half = v.size * 0.5f;
    FogBody b;
    b.kind = FogFieldKind::kVolumeTex;
    b.bound.axis[0] = Vec3f(1.0f, 0.0f, 0.0f);
    b.bound.axis[1] = Vec3f(0.0f, 1.0f, 0.0f);
    b.bound.axis[2] = Vec3f(0.0f, 0.0f, 1.0f);
    b.bound.center = v.center;
    b.bound.half_extent = half;
    b.sigma = v.sigma;
    b.albedo = v.albedo;
    b.emission = v.emission;
    for (int i = 0; i < 32; ++i) {
        b.params[i] = 0.0f;
    }
    b.params[volume_param::kCenterX] = v.center.x();
    b.params[volume_param::kCenterY] = v.center.y();
    b.params[volume_param::kCenterZ] = v.center.z();
    b.params[volume_param::kHalfX] = half.x();
    b.params[volume_param::kHalfY] = half.y();
    b.params[volume_param::kHalfZ] = half.z();
    b.params[volume_param::kDensityPow] = v.density_pow;
    b.params[volume_param::kTempScale] = v.temp_scale;
    return b;
}

// 某 OBB（团容器 / 点光源影响球）覆盖的屏幕 tile（凸包）。近平面后（任一角 w≤0）⇒
// *out_behind=true 且 out_tiles 清空，调用方按「保守全屏」处理。
//
// 步骤：8 角点投影到屏幕像素 → 屏幕凸包（convex_hull_tiles.h）→ 覆盖的 tile。
// Pre-conditions: grid_w >= 1, grid_h >= 1, tile_size > 0
inline void AppendBodyCoveredTiles(const Obb& box,
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
        Vec3f corner = box.center;
        for (int a = 0; a < 3; ++a) {
            const float sign = (a == 0) ? sx : ((a == 1) ? sy : sz);
            corner = corner + box.axis[a] * (box.half_extent[a] * sign);
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
        AppendBodyCoveredTiles(bodies[bi].bound, mvp, tile_size, grid_w, grid_h, &covered, &behind);

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

// 由**点光源列表** + 相机 MVP 生成「每 tile ≤K 个光源索引」的 tile 索引纹理（RGBA8）像素数据。
//
// 与 BuildTileFogIndexData **同机制**，只是对象是点光源（影响球）：把光源世界 AABB（球的外接
// 立方，中心=光源位置、半径=linear_radius）的 8 角投影 → 屏幕凸包 → 覆盖的 tile；
// 任一角在近平面后 ⇒ 保守覆盖全屏（与雾团剪枝同规则）。先到先得，每 tile 超过 max_per_tile 丢弃多余。
//
// ⚠️ 索引为 uint8 + sentinel ⇒ **全局至多 sentinel 个光源**（索引 0..sentinel-1）；
//    超出者直接丢弃（否则 static_cast<uint8_t> 回绕 → 指到错误光源）。
//
// 用途：fire_fog inject 的**点光源内散射**按 froxel 的 Nxy 栅格做 tile culling
//（与雾团 TC **解耦**：光源表用 fire_fog 自己的网格，不共用渲染器的 16px 光表）。
//
// 输出布局：宽 = grid_w*texels_per_tile，高 = grid_h；每 tile 的 texels_per_tile 个 texel
// 依次接排；每 texel 的 RGBA 各放一个 uint8 光索引；空槽填 sentinel。
// 返回的 vector 可直接 glTexImage2D(..., GL_RGBA, GL_UNSIGNED_BYTE, data)。
//
// Pre-conditions: grid_w >= 1, grid_h >= 1, tile_size > 0, max_per_tile == texels_per_tile*4
inline std::vector<uint8_t> BuildTileLightIndexData(const std::vector<PointLight>& lights,
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

    const int light_count =
        std::min(static_cast<int>(lights.size()), static_cast<int>(sentinel));
    for (int li = 0; li < light_count; ++li) {
        const PointLight& l = lights[li];
        if (l.linear_radius <= 0.0f) {
            continue;   // 无效范围，跳过（与雾团 radius > 0 同门）
        }
        // 光源影响球 → 外接立方 OBB（各半轴 = linear_radius）。
        Obb box;
        box.center = l.position;
        box.axis[0] = Vec3f(1.0f, 0.0f, 0.0f);
        box.axis[1] = Vec3f(0.0f, 1.0f, 0.0f);
        box.axis[2] = Vec3f(0.0f, 0.0f, 1.0f);
        box.half_extent = Vec3f(l.linear_radius, l.linear_radius, l.linear_radius);

        std::vector<TileCoord> covered;
        bool behind = false;
        AppendBodyCoveredTiles(box, mvp, tile_size, grid_w, grid_h, &covered, &behind);

        if (behind) {
            for (int t = 0; t < total_tiles; ++t) {
                uint8_t& cnt = counts[t];
                if (cnt >= static_cast<uint8_t>(max_per_tile)) {
                    continue;
                }
                idx[static_cast<size_t>(t) * max_per_tile + cnt] = static_cast<uint8_t>(li);
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
            idx[static_cast<size_t>(t) * max_per_tile + cnt] = static_cast<uint8_t>(li);
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
        AppendBodyCoveredTiles(b.bound, mvp, tile_size, grid_w, grid_h, &covered, &behind);

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

// 每 tile 的「tile 中心视线方向」（单位向量；RGBA32F，w 填 1），供 inject 用。
//
// inject 每个 froxel texel 本要自己算 tile 中心视线（两次 MVP 逆乘 + normalize），
// 但同一 tile 的 sblock² 个 texel 共享同一条视线 ⇒ 搬到 CPU 每 tile 算一次、
// 存成 Nxy×1 小纹理，inject 仅 texelFetch。方向求法与 composite 的逐像素 rd 一致
//（取 far 平面上的点 - 相机位置，再归一）。
//
// 输出布局：宽 = grid_w，高 = grid_h，逐行接排，每 tile 4 个 float (rd.x, rd.y, rd.z, 1)。
// 可直接 glTexImage2D(..., GL_RGBA, GL_FLOAT, data)。
//
// Pre-conditions: inv_vp != nullptr, grid_w >= 1, grid_h >= 1
inline std::vector<float> BuildTileRayData(const float inv_vp[16],
                                           const Vec3f& cam_pos,
                                           int grid_w,
                                           int grid_h) {
    CHECK(inv_vp != nullptr);
    CHECK_GE(grid_w, 1);
    CHECK_GE(grid_h, 1);
    std::vector<float> out(static_cast<size_t>(grid_w) * grid_h * 4, 0.0f);
    for (int tr = 0; tr < grid_h; ++tr) {
        for (int tc = 0; tc < grid_w; ++tc) {
            // tile 中心 NDC（Nxy 单元中心）。
            const float nx = ((static_cast<float>(tc) + 0.5f) / static_cast<float>(grid_w))
                             * 2.0f - 1.0f;
            const float ny = ((static_cast<float>(tr) + 0.5f) / static_cast<float>(grid_h))
                             * 2.0f - 1.0f;
            // far 平面点（ndc_z = 1）→ 世界（列主序 inv_vp）。
            const float cx = inv_vp[0] * nx + inv_vp[4] * ny + inv_vp[8] + inv_vp[12];
            const float cy = inv_vp[1] * nx + inv_vp[5] * ny + inv_vp[9] + inv_vp[13];
            const float cz = inv_vp[2] * nx + inv_vp[6] * ny + inv_vp[10] + inv_vp[14];
            const float cw = inv_vp[3] * nx + inv_vp[7] * ny + inv_vp[11] + inv_vp[15];
            const float iw = (std::fabs(cw) > 1e-12f) ? (1.0f / cw) : 1.0f;
            const float dx = cx * iw - cam_pos.x();
            const float dy = cy * iw - cam_pos.y();
            const float dz = cz * iw - cam_pos.z();
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float inv_len = (len > 1e-12f) ? (1.0f / len) : 0.0f;
            float* px = &out[(static_cast<size_t>(tr) * grid_w + tc) * 4];
            px[0] = dx * inv_len;
            px[1] = dy * inv_len;
            px[2] = dz * inv_len;
            px[3] = 1.0f;
        }
    }
    return out;
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_LOWER_H_

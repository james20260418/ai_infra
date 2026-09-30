// JPOV 局部体积雾 — CPU tile culling 实现

#include "tools/jpov/src/volumetric_fog/tile_fog_culling.h"

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/src/volumetric_fog/fog_common.h"

namespace jpov {
namespace volumetric_fog {

FogTileLayout ComputeFogTileLayout(int fbo_w, int fbo_h, int tile_size, int k_max) {
    CHECK_GT(tile_size, 0);
    CHECK_GE(k_max, 1);
    FogTileLayout l;
    l.grid_w = std::max(1, (fbo_w + tile_size - 1) / tile_size);
    l.grid_h = std::max(1, (fbo_h + tile_size - 1) / tile_size);
    l.tex_w = l.grid_w * k_max;
    l.tex_h = l.grid_h;
    return l;
}

namespace {

// 一个雾体在世界空间的包围球（center + bounding_radius）。
struct BlobBounds {
    double cx, cy, cz;
    double radius;
};

BlobBounds SphereBounds(const FogSphere& f) {
    return {f.center.x(), f.center.y(), f.center.z(),
            static_cast<double>(f.radius)};
}

// 圆柱：包围球取「柱心 + sqrt(r² + (h/2)²)」，保守 ⊇ 柱体。
BlobBounds CylinderBounds(const FogCylinder& f) {
    double ax = f.axis.x(), ay = f.axis.y(), az = f.axis.z();
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-12) {
        return {f.base.x(), f.base.y(), f.base.z(), 0.0};
    }
    ax /= len; ay /= len; az /= len;
    const double hh = 0.5 * f.height;
    const double r = f.radius;
    return {f.base.x() + ax * hh, f.base.y() + ay * hh, f.base.z() + az * hh,
            std::sqrt(r * r + hh * hh)};
}

// 把个体积雾体的屏幕覆盖矩形（像素，GL 原点左下）填入 tile 表。
// 返回 false 表示完全不覆盖（可跳过）。
bool MarkBlobTiles(const BlobBounds& b, int fbo_w, int fbo_h, const float mvp[16],
                   int tile_size, int k_max, const FogTileLayout& layout,
                   uint16_t index, std::vector<int>* counts,
                   std::vector<uint16_t>* indices) {
    const double dirs[8][3] = {{1, 1, 1},  {1, 1, -1}, {1, -1, 1},  {1, -1, -1},
                               {-1, 1, 1}, {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1}};
    double pmin_x = 1e30, pmax_x = -1e30, pmin_y = 1e30, pmax_y = -1e30;
    bool crosses_camera = false;
    for (int d = 0; d < 8; ++d) {
        const double px = b.cx + dirs[d][0] * b.radius;
        const double py = b.cy + dirs[d][1] * b.radius;
        const double pz = b.cz + dirs[d][2] * b.radius;
        const double cx = mvp[0] * px + mvp[4] * py + mvp[8] * pz + mvp[12];
        const double cy = mvp[1] * px + mvp[5] * py + mvp[9] * pz + mvp[13];
        const double cw = mvp[3] * px + mvp[7] * py + mvp[11] * pz + mvp[15];
        if (cw <= 0.0) {
            crosses_camera = true;
            continue;
        }
        const double sx = (cx / cw * 0.5 + 0.5) * fbo_w;
        const double sy = (cy / cw * 0.5 + 0.5) * fbo_h;
        pmin_x = std::min(pmin_x, sx);
        pmax_x = std::max(pmax_x, sx);
        pmin_y = std::min(pmin_y, sy);
        pmax_y = std::max(pmax_y, sy);
    }

    int min_tx, max_tx, min_ty, max_ty;
    if (crosses_camera) {
        min_tx = 0; max_tx = layout.grid_w - 1;
        min_ty = 0; max_ty = layout.grid_h - 1;
    } else {
        if (pmax_x < 0 || pmin_x > fbo_w || pmax_y < 0 || pmin_y > fbo_h) {
            return false;
        }
        min_tx = static_cast<int>(std::floor(pmin_x)) - 1;
        max_tx = static_cast<int>(std::ceil(pmax_x)) + 1;
        min_ty = static_cast<int>(std::floor(pmin_y)) - 1;
        max_ty = static_cast<int>(std::ceil(pmax_y)) + 1;
        min_tx = std::max(0, min_tx) / tile_size;
        max_tx = std::min(fbo_w - 1, max_tx) / tile_size;
        min_ty = std::max(0, min_ty) / tile_size;
        max_ty = std::min(fbo_h - 1, max_ty) / tile_size;
        min_tx = std::max(0, min_tx);
        max_tx = std::min(layout.grid_w - 1, max_tx);
        min_ty = std::max(0, min_ty);
        max_ty = std::min(layout.grid_h - 1, max_ty);
    }

    for (int ty = min_ty; ty <= max_ty; ++ty) {
        for (int tx = min_tx; tx <= max_tx; ++tx) {
            const int t = ty * layout.grid_w + tx;
            int& cnt = (*counts)[t];
            if (cnt >= k_max) {
                continue;  // 满了丢弃（先到先得）
            }
            (*indices)[static_cast<size_t>(ty) * layout.tex_w + tx * k_max + cnt] =
                index;
            ++cnt;
        }
    }
    return true;
}

}  // namespace

void BuildFogTileIndices(const std::vector<FogSphere>& spheres,
                         const std::vector<FogCylinder>& cylinders, int fbo_w,
                         int fbo_h, const float mvp[16], int tile_size, int k_max,
                         const FogTileLayout& layout,
                         std::vector<uint16_t>* indices /*output*/) {
    CHECK(indices != nullptr);
    CHECK_GT(tile_size, 0);
    CHECK_GE(k_max, 1);
    const size_t need =
        static_cast<size_t>(layout.tex_w) * static_cast<size_t>(layout.tex_h);
    CHECK_GE(indices->size(), need) << "indices 缓冲过小";
    std::fill(indices->begin(), indices->begin() + need, kFogIndexSentinel);

    const int total_tiles = layout.grid_w * layout.grid_h;
    std::vector<int> counts(static_cast<size_t>(total_tiles), 0);

    uint16_t index = 0;
    for (const FogSphere& f : spheres) {
        if (index >= kMaxFogBlobs) {
            LOG_FIRST_N(WARNING, 1) << "fog spheres 超过上限 " << kMaxFogBlobs
                                    << "，多余者忽略";
            break;
        }
        MarkBlobTiles(SphereBounds(f), fbo_w, fbo_h, mvp, tile_size, k_max, layout,
                      index, &counts, indices);
        ++index;
    }
    for (const FogCylinder& f : cylinders) {
        if (index >= kMaxFogBlobs) {
            LOG_FIRST_N(WARNING, 1) << "fog cylinders 超过上限 " << kMaxFogBlobs
                                    << "，多余者忽略";
            break;
        }
        MarkBlobTiles(CylinderBounds(f), fbo_w, fbo_h, mvp, tile_size, k_max, layout,
                      index, &counts, indices);
        ++index;
    }
}

}  // namespace volumetric_fog
}  // namespace jpov

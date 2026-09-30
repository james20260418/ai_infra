// JPOV 局部体积雾 — CPU tile culling（仿点光源 BuildTileLightIndices）
//
// 见 docs/jpov_volumetric_fog_design.md §5。把每个雾体的屏幕覆盖投影成 tile 矩形，
// 往覆盖的每个 tile 写入其 index（**先到先得，每 tile ≤ k_max，满了丢弃**）。
// 调用方**必须按重要度预排**（雾体数组顺序 = cap 时保留顺序）。
//
// 覆盖用「雾体包围球的世界 AABB 8 角投影」保守求（与灯同款；宁多勿少，漏盖会漏光）。
// 索引位宽 uint16（雾体数可 ≥ 1000；灯是 uint8 不够）。

#ifndef JPOV_SRC_VOLUMETRIC_FOG_TILE_FOG_CULLING_H_
#define JPOV_SRC_VOLUMETRIC_FOG_TILE_FOG_CULLING_H_

#include <cstdint>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/interface/fog_volume.h"

namespace jpov {
namespace volumetric_fog {

// tile 索引纹理的布局（宽 = grid_w * k，高 = grid_h；texel (tx*k+slot, ty)）。
struct FogTileLayout {
    int grid_w = 0;   // tile 列数
    int grid_h = 0;   // tile 行数
    int tex_w = 0;    // = grid_w * k_max
    int tex_h = 0;    // = grid_h
};

// 计算 tile 网格布局（不写表）。k_max 为每 tile 上限。
FogTileLayout ComputeFogTileLayout(int fbo_w, int fbo_h, int tile_size, int k_max);

// 构建 tile 索引表（CPU）。
//
//   spheres / cylinders : 雾体（按重要度预排；先处理 spheres 再 cylinders）
//   mvp                 : 相机 MVP（Proj*View，列主序），用于投影
//   indices             : 输出缓冲，大小 = layout.grid_w*k_max*layout.grid_h，按行主序
//                         （行 = ty，列 = tx*k_max + slot）；未用槽写 kFogIndexSentinel
//
// Pre-condition: indices 非空且大小 ≥ layout 所需；tile_size > 0；k_max ≥ 1
void BuildFogTileIndices(const std::vector<FogSphere>& spheres,
                         const std::vector<FogCylinder>& cylinders,
                         int fbo_w, int fbo_h, const float mvp[16],
                         int tile_size, int k_max,
                         const FogTileLayout& layout,
                         std::vector<uint16_t>* indices /*output*/);

}  // namespace volumetric_fog
}  // namespace jpov

#endif  // JPOV_SRC_VOLUMETRIC_FOG_TILE_FOG_CULLING_H_

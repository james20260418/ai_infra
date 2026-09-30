// JPOV 局部体积雾 — CPU/GPU 共享常量与属性布局
//
// 见 docs/jpov_volumetric_fog_design.md §4.3/§5。fog_pass_shader.h 的 GLSL 必须与
// 此处的布局常量**逐一对应**（改一处必须同步另一处）。

#ifndef JPOV_SRC_VOLUMETRIC_FOG_FOG_COMMON_H_
#define JPOV_SRC_VOLUMETRIC_FOG_FOG_COMMON_H_

#include <cstdint>

namespace jpov {
namespace volumetric_fog {

// 屏幕 tile 边长（像素）。仿点光源；16×16 是文档 §13.4 的甜区。
inline constexpr int kFogTileSize = 16;

// 每个 tile 的雾体上限 K（每像素 overdraw 硬上限；先到先得，调用方按重要度预排）。
inline constexpr int kMaxFogPerTile = 16;

// 雾体属性纹理：每个雾体占 kFogAttrTexels 个 RGBA32F texel（宽度 = kFogAttrTexels）。
//   texel 0: (pos.xyz, radius)
//   texel 1: (axis.xyz, height)                    # 球：radius 用 texel0.w，本行忽略
//   texel 2: (color.rgb, sigma0)
//   texel 3: (L_in.rgb, shape)                     # shape: 0=球 1=圆柱
//   texel 4: (radialProfile, axialProfile, 0, 0)
inline constexpr int kFogAttrTexels = 5;

// 属性纹理最多容纳的雾体数（高度上限）。超过者忽略（LOG warning）。
inline constexpr int kMaxFogBlobs = 4096;

// tile 索引纹理里的空槽哨兵（uint16）。
inline constexpr uint16_t kFogIndexSentinel = 0xFFFF;

// 形状编码（与 shader 的 kShapeSphere/kShapeCylinder 对应）。
inline constexpr float kFogShapeSphere = 0.0f;
inline constexpr float kFogShapeCylinder = 1.0f;

// 剖面编码（与 shader 的 kDome/kSharp/kUniform/kDomeAxial 对应）。
inline constexpr float kFogProfileDome = 0.0f;
inline constexpr float kFogProfileSharp = 1.0f;
inline constexpr float kFogProfileUniform = 2.0f;
inline constexpr float kFogProfileDomeAxial = 3.0f;

}  // namespace volumetric_fog
}  // namespace jpov

#endif  // JPOV_SRC_VOLUMETRIC_FOG_FOG_COMMON_H_

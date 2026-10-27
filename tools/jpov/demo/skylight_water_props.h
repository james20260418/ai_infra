// JPOV 天光查看器 —— 水面场景的「透射验证」道具（GL-free 布局 + 材质）
//
// 目的（2026-10-27 Danis）：隔着水皮看水下的东西，验证透射是不是对的。
// 道具从下往上堆成一条**穿水线**的塔：
//
//     人（站在桌面，全在水上）              y = 0.55 … 2.30
//     桌子（桌腿跨过水线：桌面 0.55、桌脚 −0.20）   y = −0.20 … 0.55
//     小 box（**全在水下**）                y = −1.00 … −0.20
//     大 box（**全在水下**，顶面 −1.00）      y = −5.00 … −1.00
//     —— 水面（水皮 quad）——               y = 0
//
// 为什么这么摆：
//   * 两件水下物（大 / 小 box）**颜色不同**：隔着水皮应当都能看见、且都被水膜按同一条
//     覆盖率淡掉一层 —— 一样地"褪"，而不是其中一件消失；
//   * 桌腿**跨过水线**：水线附近有没有硬切换 / 抖动 / 补丁，一眼就能看出来；
//   * 水面之上（人、桌面）与水之下（两件 box）在同一屏里，A/B 关水时"水下部分怎么变"
//     就是透射本身的效果。
//
// 坐标系：y-up、水面 y = kWaterPlaneY（= 0，定义在 skylight_water_quad.h）。
// 水下**没有海底**：这些 box 就悬在水里（本步只看光学，不做水下几何）。

#ifndef JPOV_DEMO_SKYLIGHT_WATER_PROPS_H_
#define JPOV_DEMO_SKYLIGHT_WATER_PROPS_H_

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_water_quad.h"

namespace jpov_skylight {

// ── 大 box（水下最底层）──
inline constexpr float kWaterBigBoxHalfXZ = 3.00f;   // 占地 6 × 6 m
inline constexpr float kWaterBigBoxHalfY  = 2.00f;   // 高 4 m
inline constexpr float kWaterBigBoxTopY   = -1.00f;  // 顶面在水下 1 m

// ── 小 box（坐在大 box 顶面）──
inline constexpr float kWaterSmallBoxHalfXZ = 1.20f;   // 占地 2.4 × 2.4 m
inline constexpr float kWaterSmallBoxHalfY  = 0.40f;   // 高 0.8 m
inline constexpr float kWaterSmallBoxBottomY = kWaterBigBoxTopY;          // = −1.00
inline constexpr float kWaterSmallBoxTopY    = kWaterSmallBoxBottomY + 2.0f * kWaterSmallBoxHalfY;  // = −0.20

// ── 桌子（桌脚坐在小 box 顶面，桌面高 0.75 m）──
inline constexpr float kWaterTableHeight = 0.75f;
inline constexpr float kWaterTableTopY   = kWaterSmallBoxTopY + kWaterTableHeight;   // = 0.55

// ── 人（脚踩桌面）──
inline constexpr float kWaterPersonFeetY = kWaterTableTopY;   // = 0.55

// 大 box 中心（世界系）：顶面 = center.y + halfY。
inline jpov::Vec3f WaterBigBoxCenter() {
    return {0.0f, kWaterBigBoxTopY - kWaterBigBoxHalfY, 0.0f};
}

// 小 box 中心：底面 = center.y − halfY。
inline jpov::Vec3f WaterSmallBoxCenter() {
    return {0.0f, kWaterSmallBoxBottomY + kWaterSmallBoxHalfY, 0.0f};
}

// 水下两件 box 的材质：**故意取两个明显不同的色相**，好判断"透射是否对两件一样生效"。
// 粗糙度都拉到 0.9 —— 水下部分要看的不是高光，而是"还能不能认出它是什么颜色"。
inline jpov::PBRMaterial WaterBigBoxMaterial() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.30f, 0.34f, 0.40f, 1.0f},   // 深蓝灰
        /*metallic*/ 0.0f,
        /*roughness*/ 0.90f);
}

inline jpov::PBRMaterial WaterSmallBoxMaterial() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.62f, 0.34f, 0.20f, 1.0f},   // 砖红（最能和水的青色拉开）
        /*metallic*/ 0.0f,
        /*roughness*/ 0.90f);
}

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_WATER_PROPS_H_

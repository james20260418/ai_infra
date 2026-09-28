// JPOV BurningCommand —— 「燃烧」特效命令（物体表面着火）
//
// 与「火焰 FireCommand」的区别（见 docs/jpov_effect_pass_design.md）：
//   - 火焰 = 自足发光体，锚在**一个点**（炉灶/火把/篝火）。
//   - 燃烧 = **物体的状态**，火源由**物体几何**驱动、**跟着物体走**。
//
// 因此本命令不含「尺寸」，而含**物体的世界摆放 + 半尺寸**：渲染侧据此在物体表面
// 采样火舌簇（底部一圈 + 沿竖直往上蔓延），并绘制火 + 烟。

#ifndef JPOV_BURNING_COMMAND_H_
#define JPOV_BURNING_COMMAND_H_

#include <cstdint>

#include "tools/jpov/interface/pbr_material.h"   // Color
#include "geom/common/vec.h"

namespace jpov {

using Vec3f = geom::Vec3<float>;

// 一个「燃烧体」：某物体正在燃烧。
struct BurningCommand {
    // ---- 物体几何（世界摆放，语义与 Object3DCommand 的 center/up/front 一致）----
    Vec3f center;                 // 物体中心世界坐标（火随物体移动）
    Vec3f up;                     // 物体局部 +Y 的世界方向（通常 {0,1,0}）
    Vec3f front;                  // 物体局部 +Z 的世界方向
    Vec3f half_extents;           // 物体半尺寸（x/y/z，米）—— 火源分布的依据

    // ---- 燃烧程度 ----
    float strength = 1.0f;        // 0~1：火势（影响火舌数量 + 爬升高度 + 强度）
    uint32_t seed = 0;            // 确定性种子（同 seed/参数 → 同分布，可复现）

    // ---- 外观 ----
    Color color_core  = {1.00f, 0.86f, 0.48f, 1.0f};   // 火舌核心色（亮）
    Color color_outer = {0.95f, 0.16f, 0.02f, 1.0f};   // 火舌外焰色（暗红）
    Color smoke_color = {0.32f, 0.31f, 0.30f, 0.34f};  // 烟（灰、低不透明度）

    float intensity = 1.2f;       // 火光强度（HDR 乘子）
    float speed = 1.0f;           // 动画速度倍率

    // Pre-condition: half_extents 各分量 > 0
    // Pre-condition: strength >= 0
};

}  // namespace jpov

#endif  // JPOV_BURNING_COMMAND_H_

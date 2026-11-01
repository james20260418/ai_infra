// JPOV 点光源数据纹理 — 三份 PBR 片元共享的「每盏灯 9 个 float」
//
// 为什么不再用 `uniform Light uLights[MAX_TOTAL_LIGHTS]`（2026-11-01 Windows 真卡实测）：
//   **NVIDIA 片元级的常量寄存器上限是 1024 个 vec4**。它把 `Light` 按 4 个寄存器打包，
//   255 盏灯要 1020 个，再加级联阴影 / PCF / 环境镜面立刻超，链接直接失败：
//
//     error C6020: Constant register limit exceeded at "uHasSun";
//                  more than 1024 registers needed to compile program
//
//   Mesa/llvmpipe 把同一个结构体打包成 3 个寄存器（765 个）⇒ 本地一直是绿的，只有真卡
//   才暴露。这不是「把上限调小」能糊过去的：只要数据留在默认 uniform 块里，NVIDIA 上
//   可用光源数就被死死限制在 ~200 盏以内，而且以后再加任何 uniform 都会重新踩线。
//
//   所以光源数据改成**数据纹理**（同一套 fetch 在 GL 3.3 全平台可用，纹理不吃常量寄存器；
//   顺便把原来「每盏灯 × 5 次 glUniform 调用 × 每帧 × 4~8 个 program」的上传，
//   降成一次 glTexSubImage2D）。
//
// 布局：一张 RGBA32F 纹理，宽 = 光源下标（0..kMaxPointLights-1），高 = 3 行
//   row 0 = (position.xyz, linear_radius)
//   row 1 = (color.rgb,    intensity)
//   row 2 = (physical_radius, 0, 0, 0)
// 对应 shader 里的 ReadLight()（object3d_renderer.h / instanced_object_renderer.h）。
// ⚠️ 改这里的行序/列义必须同步改那两处 ReadLight；它们各自有注释指向本文件。

#ifndef JPOV_SRC_LIGHTS_TEXTURE_H_
#define JPOV_SRC_LIGHTS_TEXTURE_H_

#include "tools/jpov/interface/render_command.h"

namespace jpov {

class ShaderManager;

// 单帧最多参与渲染的点光源数。与两个渲染器的 kMaxTotalLights / tile 索引编码
// （kLightIndexSentinel = 255）必须一致。
inline constexpr int kMaxPointLights = 255;

// 数据纹理的行数（见本文件顶部的布局）。
inline constexpr int kLightDataRows = 3;

// 本帧实际参与渲染的光源数（= min(point_lights.size(), kMaxPointLights)）。
int ClampedPointLightCount(const RenderCommandList& cmds);

// 幂等创建数据纹理（*tex == 0 时创建；已存在则什么都不做）。
// 内容全部由 UploadLightDataTexture 写，创建时不必给初值。
// Pre-condition: GL context 已激活；tex != nullptr
void EnsureLightDataTexture(unsigned int* tex /*inout*/);

// 把本帧的光源写进数据纹理（只更新前 ClampedPointLightCount() 列）。
// Pre-condition: EnsureLightDataTexture 已调用且 tex != 0
void UploadLightDataTexture(const RenderCommandList& cmds, unsigned int tex);

// 把数据纹理绑到 kTexUnitLightData 并给 prog 设 `uLightData` 采样器单元，
// 同时上传与本帧光源数相关的两个普通 uniform（uTotalLights / uTileCulling）。
//
// 为什么要按 program 逐个调用：GL 的 sampler uniform 是 program 状态（不是全局状态），
// 而 PBR 有多个 program（常规 / full / cutout / 实例各一套）。
// Pre-condition: tex != 0；light_count ∈ [0, kMaxPointLights]
void BindLightDataTexture(ShaderManager* shader_mgr, unsigned int prog,
                          unsigned int tex, int light_count, bool tile_culling);

}  // namespace jpov

#endif  // JPOV_SRC_LIGHTS_TEXTURE_H_

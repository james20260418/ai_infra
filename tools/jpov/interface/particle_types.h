// JPOV 广告牌粒子 — 接口层类型（GL-free，被 render_command 与 src/particles 共用）
//
// 这里只放「模拟产出、渲染消费」的**纯数据**：广告牌朝向/混合/排序枚举 + 单个广告牌
// 实例。模拟内核（src/particles/particle_system.h）产出 ParticleInstance；渲染命令
// （interface/render_command.h 的 ParticleRenderCommand）承载一批实例。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。

#ifndef JPOV_INTERFACE_PARTICLE_TYPES_H_
#define JPOV_INTERFACE_PARTICLE_TYPES_H_

#include <cstdint>

#include "geom/common/vec.h"
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

// 广告牌朝向调制（见设计 §11.2）。
enum class AlignMode : uint8_t {
    kView = 0,      // 面向相机平面（用相机 right/up）
    kVertical = 1,  // 竖直/roll 锁定（只绕 Y 轴转向相机）—— 火的关键
    kFacing = 2,    // 面向相机位置（球面 facing）
    kVelocity = 3,  // 沿速度方向对齐（火花/余烬）
};

// 混合模式。
enum class BlendMode : uint8_t {
    kAdditive = 0,  // 加法（发光焰/火花）
    kAlpha = 1,     // 半透明（烟）
};

// 排序模式。
enum class SortMode : uint8_t {
    kNone = 0,      // 不排序（加法层无顺序依赖）
    kByDepth = 1,   // 按视深背→前排序（半透明层）
};

// 一个已解算好的广告牌面片（模拟产出，渲染消费）。
struct ParticleInstance {
    geom::Vec3<float> position;  // 世界位置（面片中心）
    geom::Vec3<float> velocity;  // 当前速度（供 velocity 对齐 / 拉伸用）
    float width = 0.0f;          // 世界宽度
    float height = 0.0f;         // 世界高度
    float rotation = 0.0f;       // 面片平面内滚转角（弧度）
    Color color;                 // RGBA（已含生命期颜色/不透明度）
    float frame = 0.0f;          // flipbook 帧号（已解算，含相位/循环）
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_PARTICLE_TYPES_H_

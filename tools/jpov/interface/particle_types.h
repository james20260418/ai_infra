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

    // 自适应：侧视 ≈ kVertical（roll 锁定，火苗朝上），俯视 → kView（否则竖直卡
    // 被看成一条扁线）。按 t = |dot(normalize(toCam), world_up)| 在两者之间插值基向量：
    //   t=0（平视）→ 纯 kVertical；t→1（正俯视）→ 纯 kView；
    // 插值区间 smoothstep(0.55, 0.95, t)（俯视 25° 内才开始切换，避开过渡区抖动）。
    // 用途：一个模式覆盖「侧视好看 + 俯视不穿帮」，代价是过渡区基向量非刚性（已正交化）。
    kViewVerticalBlend = 4,
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
    float age01 = 0.0f;          // 归一化年龄 [0,1]（0=出生，1=死亡）——供温度着色用
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_PARTICLE_TYPES_H_

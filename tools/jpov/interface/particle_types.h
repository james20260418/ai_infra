// JPOV 广告牌粒子 — 接口层类型（GL-free，被 render_command 与 src/particles 共用）
//
// 这里只放「模拟产出、渲染消费」的**纯数据**：广告牌朝向/混合/排序枚举 + 单个广告牌
// 实例。模拟内核（src/particles/particle_system.h）产出 ParticleInstance；渲染命令
// （interface/render_command.h 的 ParticleRenderCommand）承载一批实例。
//
// ============================ 数据流（四步） ============================
//   ① 模拟：ParticleEmitter::Update() 逐粒子解算位置/尺寸/颜色/帧号 → ParticleInstance
//   ② 组批：用例把一批实例 + 整批渲染设置塞进一条 ParticleRenderCommand
//   ③ 累积：ParticleRenderer 用一条 instanced draw 画进 **1/2 分辨率 WBO 纹理**
//        （accum / reveal / depth 三附件，顺序无关）
//   ④ 合成：Composite() 把 WBO 解析成「颜色 + 覆盖率」，以**预乘 over** 写回 HDR
//
// ============================ 像素怎么变成 over / add ============================
// 混合模式是**每条 ParticleRenderCommand 一个**（ParticleRenderCommand::blend），
// 既不在贴图里、也不逐粒子。同一张 flipbook 完全可以被两条命令分别按加法 / over 用。
//
// 记号（全部在片元着色器里，见 src/particles/particle_renderer.h 的 kFsWbo）：
//     tex    = flipbook 采样出的 texel（RGBA ∈ [0,1]；命令没贴图时 = (1,1,1,1)）
//     col    = ParticleInstance.color × tex        （逐分量相乘）
//     亮度    = max(col.r, col.g, col.b)
//
//   kAdditive（火 / 火花 / 闪电 —— 发光叠加）
//       a = 亮度 × col.a = max(col.rgb) × col.a
//       语义：texel 的**亮度**给出形状/强度（黑底 = 全透），**color.a 是整体不透明度旋钮**
//       （梯度两端为 0 ⇒ 出生淡入 / 生命末淡出）。
//       为什么用亮度：CC0 火焰序列帧普遍是「黑底 + alpha 全 255」
//       （如 assets/fire/candidates/cand10_hwrow.png），只看 alpha 会把整个方形卡片
//       画成一块黑板。
//       ⇒ 加法模式下：要更亮改 col.rgb（= color.rgb × texel.rgb）；要更浓/更透改 color.a。
//       ⇒ **color.rgb 可以 > 1（HDR）**：因为 a 只取 `clamp(max(col.rgb),0,1) × col.a`，
//          抬 rgb 只增亮、不增遮蔽 —— 「亮而透」的火焰就是这么调出来的（rgb 5~7 + alpha 0.15）。
//
//   kAlpha（烟 / 尘 / 雾团 —— 遮挡背景）
//       a = col.a = color.a × texel.a               ← **透明度唯一的定义就是它**
//       语义：a 就是覆盖率，合成时按 a 与背景做 over。
//       ⇒ 半透明模式下要更浓改 color.a（或贴图 alpha）；col.rgb 只决定颜色。
//
// 两种模式共同的片元规则：
//   * a ≤ 1/255 → discard（不写 WBO、不吃带宽）。
//   * 与**不透明场景深度**比较：片元视深 z > 场景深度 + 0.02 米 → discard（被物体挡住）。
//     当前只有这个 0.02 米的硬判定，**没有软粒子淡出**（ParticleRenderCommand 无此旋钮）。
//   * WBO 权重 w = max(min(1, 亮度·a), a) · clamp(0.03/(1e-5+(z/5)^4), 1e-2, 3e3)，
//     近处优先（重标定自 McGuire 2013 的 z/5）。
//
// ============================ 这个接口**没有**的东西 ============================
//   * 没有「整批整体透明度 / 淡入淡出」旋钮：alpha 完全由逐实例 color（+ 贴图）决定，
//     整批淡出只能改梯度曲线或逐实例 color。要在命令上加 opacity 需另行设计。
//   * 没有 per-particle 混合：一个粒子只能属于一条命令 ⇒ 一种混合模式；要混用就发两条命令。
//   * 没有逐命令排序：渲染走 WBO，顺序无关（见 SortMode）。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。

#ifndef JPOV_INTERFACE_PARTICLE_TYPES_H_
#define JPOV_INTERFACE_PARTICLE_TYPES_H_

#include <cstdint>

#include "geom/common/vec.h"
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

// 广告牌朝向调制 —— 决定面片的 (right, up) 局部基向量（见设计 §11.2）。
// **JPOV 的统一默认是 kView**；其余模式按需要显式选。
//
// 记：P = 粒子世界位置，C = 相机位置，toCam = normalize(C − P)。
//     up 是面片的「高」轴（宽度沿 right），width/height 都按这个基展开。
//
//   kView     right = 相机 right，up = 相机 up
//             卡片严格平行于相机平面；相机 roll（翻滚）会带着卡片一起扭。
//
//   kVertical up = 世界 +Y（固定），right = normalize(cross(worldUp, toCam))
//             只绕世界 Y 轴转向相机：卡片永远竖直、不翻滚。（可选模式；JPOV 里**默认用 kView**）
//             视线与 +Y 近平行时 right 退化 → 回退到相机 right。
//
//   kFacing   n = toCam（卡片法线 = 朝相机点），right = normalize(cross(worldUp, n))，
//             up = normalize(cross(n, right))
//             球面 facing：离屏幕中心越远，卡片越朝向该点（比 kView 更有体积感）。
//
//   kVelocity up = normalize(velocity)（速度 ≈ 0 时退化为相机 up），
//             right = normalize(cross(up, toCam))
//             卡片平面**包含速度**、法线朝相机 —— 火花 / 雨丝 / 曳光弹。
//             局部 Y = 速度方向 ⇒ stretched 拉长就是沿速度（见 ParticleRenderCommand::stretched）。
//             退化保护：速度 ∥ 视线时依次回退 cross(up, camRight) → cross(up, camUp)。
//
// ⚠️ **stretched 只对 kVelocity 有意义**：stretch 永远作用在「滚转后的局部 Y」上；
//    对 kView / kVertical，局部 Y 是「相机上 / 世界上」，拉出来是「高个儿 sprite」，
//    不是物理上的拉丝 —— 不要那样用。
enum class AlignMode : uint8_t {
    kView = 0,      // 面向相机平面（相机朝向基）
    kVertical = 1,  // 竖直/roll 锁定（只绕 Y 轴转向相机）—— 火的关键
    kFacing = 2,    // 面向相机位置（球面 facing）
    kVelocity = 3,  // 沿速度方向对齐（火花/余烬）；唯一适合 stretched 的模式
};

// 混合模式（**每条命令一个**）。详细公式见本文件顶部的「像素怎么变成 over / add」。
enum class BlendMode : uint8_t {
    kAdditive = 0,  // 加法：覆盖 = texel 亮度 × color.a（黑底即透明）；color.a 仍控制淡入淡出
    kAlpha = 1,     // 半透明：覆盖 = color.a × texel.a，按它 over 背景
};

// 排序模式。
// ⚠️ 当前**不被任何渲染器读取**：ParticleRenderer 走 WBO（加权混合、顺序无关），
//    天然不需要排序。字段保留只是接口占位，改它不会产生任何视觉效果。
//    （旧设计里半透明层要背→前排序，那是逐片混合路线的需求，本实现不走那条路。）
enum class SortMode : uint8_t {
    kNone = 0,      // 不排序（当前实际行为恒等于此）
    kByDepth = 1,   // 按视深背→前排序（预留，无实现）
};

// 一个已解算好的广告牌面片（模拟产出，渲染消费）。
// 一条命令内的所有实例共享该命令的贴图 / 网格 / 朝向 / 混合等设置（见 ParticleRenderCommand）。
struct ParticleInstance {
    geom::Vec3<float> position;  // 世界位置（米），面片中心
    geom::Vec3<float> velocity;  // 当前速度（米/秒）。只用于 kVelocity 对齐与 stretched；
                                 // 不参与光照（粒子是 unlit 的）
    float width = 0.0f;          // **半宽**（世界米）：面片总宽 = 2 × width
    float height = 0.0f;         // **半高**（世界米）：面片总高 = 2 × height
    float rotation = 0.0f;       // 面片平面内滚转角（弧度）。基向量变为
                                 //   x' = right·cosθ + up·sinθ，y' = −right·sinθ + up·cosθ，
                                 //   贴图随局部轴一起转；stretched 的拉长轴也随之转
    Color color;                 // 逐粒子 RGBA 乘子，渲染时与 texel 逐分量相乘：col = color × tex。
                                 //   rgb = 颜色 / 发光色；a = 不透明度（两种模式都生效：
                                 //   kAlpha 直接当覆盖率；kAdditive 乘在「亮度覆盖」上，控制淡入/淡出）
    float frame = 0.0f;          // flipbook 帧号（已解算，0 起）。必须落在
                                 //   [0, tiles_x × tiles_y)；可带小数 ⇒ 与下一帧线性混合
                                 //   （命令的 frame_blend 控制）
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_PARTICLE_TYPES_H_

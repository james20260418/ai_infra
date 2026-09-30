// JPOV 局部体积雾 — 公共数据模型（命令层可见）
//
// 见 docs/jpov_volumetric_fog_design.md。用户像摆点光源一样在场景里摆「雾体」：
//   - FogSphere   球雾（点状雾团 / 发光体周围的空气光）
//   - FogCylinder 圆柱雾（光柱 / 烟囱 / 竖直烟尘）
//
// 设计原则（§4.2）：用户**只**配「颜色 / 尺寸 / 强度」这类直观量；一切观感/性能
// 风险量（密度剖面 profile、相位 g、深度策略、混合、tile 尺寸、K）由 JPOV 锁定。

#ifndef JPOV_INTERFACE_FOG_VOLUME_H_
#define JPOV_INTERFACE_FOG_VOLUME_H_

#include <cstdint>

#include "geom/common/vec.h"
#include "tools/jpov/interface/camera.h"   // 提供 jpov::Vec3f 别名
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

// 密度剖面 —— JPOV **锁定菜单**（用户不应更改；见设计文档 §6）。
// 保闭式的唯一窍门：剖面写成「距离平方的多项式」，沿视线弦有解析积分。
enum class FogProfile : uint8_t {
    // ---- 径向（球 / 圆柱，u = ρ/r）：----
    kDome = 0,    // 1 − u²：中心最浓、到表面平滑归零（抛物面）。**默认**。
    kSharp = 1,   // (1 − u²)²：更「芯化」，边缘更快淡出。
    // ---- 圆柱轴向（u_y = |y|/h）：----
    kUniform = 2, // 1：柱身均匀、**硬端**（仅贴结构 / 被遮时用）。
    kDomeAxial = 3,  // 1 − u_y²：对称软端（圆柱轴向右选）。
};

// 球形体积雾（点状雾团）。
//
//   center    摆放位置（世界坐标）
//   radius    尺寸（米，> 0）
//   color     内散射色（介质 albedo，RGB ∈ [0,1]；与场景光照相乘决定雾的自发光色）
//   intensity 峰值消光系数 σ₀（1/米，> 0，可 > 1）；越大越浓（更遮挡、也散射更多）
//   profile   径向剖面（锁定菜单，默认 kDome）
struct FogSphere {
    Vec3f center = {0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    FogProfile profile = FogProfile::kDome;
};

// 圆柱体积雾。
//
//   base      底面圆心（世界坐标）
//   axis      轴向（单位向量；约定指向"向上"，柱体沿 +axis 从 base 延伸 height）
//   radius    半径（米，> 0）
//   height    高度（米，> 0）
//   color     内散射色
//   intensity 峰值消光系数 σ₀（1/米）
//   radial_profile   径向剖面（kDome / kSharp）
//   axial_profile    轴向剖面（kUniform / kDomeAxial）
//
// 密度 = σ₀ · f_r(ρ²/R²) · f_y(y²/h²)（分离式乘积；角上双零 → 棱角削圆，见 §6.5）。
struct FogCylinder {
    Vec3f base = {0.0f, 0.0f, 0.0f};
    Vec3f axis = {0.0f, 1.0f, 0.0f};
    float radius = 1.0f;
    float height = 1.0f;
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    FogProfile radial_profile = FogProfile::kDome;
    FogProfile axial_profile = FogProfile::kDomeAxial;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_FOG_VOLUME_H_

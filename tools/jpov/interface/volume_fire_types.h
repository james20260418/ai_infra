// JPOV 体积火（逐像素 raymarch）— 接口层类型（GL-free）
//
// 一团「燃料火」= 一个 3D 体密度场（`.jpovvol`：R=density∈[0,1]、G=temperature 开尔文[0,2000]），
// 运行时沿视线在该团的**世界 AABB 盒**内逐像素积分，把结果作为**一个薄层**写进 WBO 透明纹理。
//
// 设计文档：tools/jpov/docs/jpov_volumetric_fire_design.md
//
// 物理（辐射传输，emission–absorption）：沿光线积分
//     L = ∫ T(t)·σ(t)·c(t) dt ,  T(t) = exp(−∫σ ds)
// 其中 σ(density) = density · sigma_scale（1/m）；c = Blackbody(T_kelvin) · emission_scale。
// 「期望深度」z_repr = ∫ z·σ·T dt / ∫ σ·T dt（不调参，由积分直接给出）。

#ifndef JPOV_INTERFACE_VOLUME_FIRE_TYPES_H_
#define JPOV_INTERFACE_VOLUME_FIRE_TYPES_H_

#include <cstdint>

#include "geom/common/vec.h"

namespace jpov {

using Vec3f = geom::Vec3<float>;

// 一团体积火的绘制命令（世界空间，一次性描述整团）。
struct VolumeFireCommand {
    // 体纹理 id —— 由 VolumeFireRenderer::RegisterVolume 注册（0 = 不画）。
    uint32_t volume_id = 0;

    // 该团在世界空间的**轴对齐包围盒**（盒内才步进；盒外像素直接丢弃）。
    // 火体被映射到该盒：局部 [0,1]^3 → 世界 [aabb_min, aabb_max]。
    Vec3f aabb_min = {-0.5f, -0.5f, -0.5f};
    Vec3f aabb_max = {0.5f, 0.5f, 0.5f};

    // 消光尺度：σ = density(∈[0,1]) × sigma_scale，单位 1/m（与雾的 sigma 同量纲）。
    float sigma_scale = 1.0f;
    // 发射强度尺度（乘到黑体色上）。
    float emission_scale = 1.0f;
    // 温度通道归一化值 [0,1] → 开尔文 = G × temp_kelvin_scale（默认 2000K）。
    float temp_kelvin_scale = 2000.0f;

    // 播放帧号（含小数 → 相邻帧线性混合）。范围 [0, nframes)。
    float frame = 0.0f;

    // 沿光线的步进采样数（质量/性能主旋钮；典型 32~128）。
    int steps = 64;
    // 抖动：给每像素一个随机相位，把固定步长的「层状条带」换成蓝噪声（不降噪，只换形态）。
    bool jitter = true;

    // 调试：用全屏 quad 代替盒光栅化（每像素自算射线、打不中就 discard）。
    // 用来二分定位「伪影是否来自盒光栅化/剔面」。默认 false。
    bool debug_fullscreen = false;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_VOLUME_FIRE_TYPES_H_

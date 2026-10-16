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
    // 温度通道归一化值 [0,1] → 开尔文：T = temp_kelvin_min + (temp_kelvin_scale − min)·G。
    // **必须有下界**：flames 场分布偏低（G 中位数 ~0.1），若 T=G·scale 则绝大多数体素
    // 落到黑体下限（1000K）→ 整团同温同色、没有“边红芯亮”的层次。
    float temp_kelvin_scale = 3500.0f;   // 温度上界（G=1 → 此值）
    float temp_kelvin_min = 1000.0f;     // 温度下界（G=0 → 此值）
    // 【发射强度随温度】真实黑体辐射亮度 ∝ T⁴（Stefan-Boltzmann）。黑体**色**（Blackbody()）
    // 被归一化到 [0,1]，只给色相、不给亮度 → 若不自乘一个随温度增长的强度，
    // 热的芯与冷的边一样亮 → 看起来是一团均匀蜡黄、没有白热芯。
    //   实际发射 = Blackbody(T) · emission_scale · (T / temp_kelvin_ref) ^ temp_emission_pow
    // 物理上 pow=4；ref 为归一参考温度（此处强度 = emission_scale）。
    float temp_emission_pow = 4.0f;
    float temp_kelvin_ref = 1000.0f;

    // 【色谱 / Fire Color Ramp】nullptr 或空 = 用黑体；否则指向一个 1D LUT PNG 的路径
    // （由「强度/温度→颜色」色谱烘成，见 tools/jpov/genVolume/palettes/ + make_palette.py）。
    // 与 EmberGen 的 Color Gradient / Houdini 的 Fire Color Ramp 同构：色相来自查表、
    // 亮度仍由 emission_scale·(T/ref)^pow 控制。LUT 为 sRGB，shader 里转线性后用。
    // 生命周期：由调用者在 Draw 期间保持有效（通常指向 app 持有的 std::string）。
    const char* palette_png = nullptr;
    // 色谱映射区间：ramp 坐标 = clamp((G - lo)/(hi - lo), 0, 1)。默认 [0,1]（G 直接用）。
    // 参考色带的像素是按「亮度分位」抽的，而 G(flames) 与亮度不严格等价——
    // 用 [lo,hi] 把 G 的实际范围对齐到色谱两端。
    float palette_ramp_lo = 0.0f;
    float palette_ramp_hi = 1.0f;

    // 播放帧号（含小数 → 相邻帧线性混合）。范围 [0, nframes)。
    float frame = 0.0f;

    // 沿光线的步进采样数（质量/性能主旋钮；典型 32~128）。
    int steps = 64;
    // 抖动：给每像素一个随机相位，把固定步长的「层状条带」换成蓝噪声（不降噪，只换形态）。
    bool jitter = true;

    // 调试：用全屏 quad 代替盒光栅化（每像素自算射线、打不中就 discard）。
    // 用来二分定位「伪影是否来自盒光栅化/剔面」。默认 false。
    bool debug_fullscreen = false;

    // ---- 烟：让「非火」部分在纯黑背景下可见（受光散射）----
    //
    // 3 通道体纹理（R=火密度, G=温度, B=烟密度）；2 通道文件则无烟。
    // 烟只**吸收 + 受光散射**（不自发光）：纯黑背景下不散射就不可见。
    // 受光近似为**均匀照明**（环境散射光；物理上还有个火光照明的方向性分量，留作后续）：
    //     L_scatter = smoke_scatter · σ_smoke · albedo · light_color
    // light_color 取中性偏冷灰（参照官方渲染：烟呈灰蓝，与暖色火焰拉开）。
    // smoke_scatter = 0 时整条散射路径关闭（**零回归**）。
    Vec3f smoke_albedo = {0.72f, 0.70f, 0.68f};   // 烟的反照率（中性偏暖灰）
    float smoke_scatter = 0.0f;                   // 散射强度增益（0 = 关）
    // 烟消光倍数：只作用于 σ_smoke（= B·sigma_scale·此值）。烟太厚会盖住火；
    // 散射注入 ∝ σ_smoke，故这一个旋钮同时压低「烟厚」与「烟亮」。0 = 无烟。
    float smoke_sigma_scale = 1.0f;
    Vec3f light_color = {0.62f, 0.66f, 0.75f};    // 烟的受光辐射色（中性偏冷，含强度）
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_VOLUME_FIRE_TYPES_H_

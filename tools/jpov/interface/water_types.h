// JPOV 水面（全局水/海）接口层配置 —— GL-free，render_command 与 src/water 共用
//
// 一句话：一片**全局水平面**（y-up，世界 y = level），它在渲染上不是"一个物物体"，
// 而是「一个界面 + 一段介质」：
//   · 界面：菲涅尔反射（第一版只映天光——天空 cubemap 探针）+ 透射；
//   · 介质：看下去时，水下那段视线按 Beer-Lambert 吸收/散射（这就是"水下遮蔽感"）。
// 相机沉到水下时，另走一遍**整屏介质** pass（src/water/water_renderer.h）。
//
// 与 WBO 的关系（**故意不进去**）：
//   WBO 解决"一堆碎片谁挡谁"（乱序累积 → 一次解析成 (预乘色, 透射率)）。
//   水是**单层平面**（没有排序问题），但它要**改写背景**（吸收背景色 / 将来折射背景 UV），
//   而 WBO 的 accum/reveal 只能叠在已成画面上、且不保存逐层深度 ⇒ 表达不了。
//   所以水面是一个独立的"界面 pass"，**画在三个 WBO pass 之前**（粒子 / 体积火 / 条带）。
//
// 位置（Render() 里，见 src/renderer.cc）：
//   不透明 3D →【抓帧拷贝 color+depth】→【水面 pass】→ 粒子 → 体积火 → 条带
//   →【相机在水下时：整屏介质 pass】→ 远景雾 → froxel 雾 → highlight → bloom → tone map
//
// ⚠️ 水面画在 WBO 之前的**代价**（已知取舍，Danis 2026-10-27 定）：
//   水下那些粒子/火/条带会被水面写进 MRT#1 的深度剔掉（它们自己不写深度，只能靠
//   手动作深度测试，会被判定"在水面之后"）。要保住它们就得把水面挪到透明 pass 之后，
//   那会反过来让"水面之上的效果"被水吃掉，且吸收算不准 —— 本版不做。

#ifndef JPOV_INTERFACE_WATER_TYPES_H_
#define JPOV_INTERFACE_WATER_TYPES_H_

#include "geom/common/vec.h"

namespace jpov {

using Vec3f = geom::Vec3<float>;

// 全局水面参数。所有距离单位 = 米，颜色 = 线性 HDR（tone map 之前）。
//
// 默认值 = "一池清水"（吸收偏低、散射偏青），可在查看器里调。
struct WaterSurface {
    // 水面世界高度（y-up）。整个水平面 = { y == level }，向四周无限延伸
    //（渲染时用「相机为中心的极坐标 LOD 网格」铺到远平面附近，见 water_renderer.h）。
    float level = 0.0f;

    // ---- 界面：天光反射（第一版只有这个反射源）----
    // 天光消费量 [0,1]：0 = 完全不反射（只剩透射），1 = 全额消费天空探针。
    float sky_scale = 1.0f;
    // 正入射反射率 F0（水的 IOR 1.333 ⇒ ~0.02）。掠射按 Schlick 升到 1。
    float f0 = 0.02f;
    // 反射采样用的粗糙度（→ cubemap mip）。0.02 ≈ 镜面；高浪时可略调大做柔化。
    float roughness = 0.02f;

    // ---- 介质：水下吸收 + 散射（Beer-Lambert，逐通道）----
    // 吸收系数（1/米，RGB）。红光衰减最快 ⇒ 越深越青蓝。
    // 默认取"清海水"一档：3~4 m 深度下仍能看清水下结构，但已经明显偏青。
    Vec3f absorption = {0.32f, 0.080f, 0.050f};
    // 散射色（线性 HDR）。深水 = 环境光被水分子散射回来的颜色，决定"深海的底噪亮度"。
    Vec3f scatter = {0.010f, 0.055f, 0.080f};
    // 散射强度倍数（0 = 只吸收不散射，深处会变纯黑；1 = 用上面给的色）。
    float scatter_scale = 1.0f;

    // ---- 波浪（第一版 amplitude = 0 ⇒ 严格平水面；为"高浪"光栅化底座预留）----
    // 顶点位移幅度（米）。0 = 不位移（零开销零回归）。
    float wave_amplitude = 0.0f;
    // 基波长（米）。三波叠加按 1 / 1.4 / 1.8 倍波长递减、振幅递减。
    float wave_length = 24.0f;
    // 主浪方向（水平面内的单位向量；非零即可，内部会重新归一）。
    Vec3f wave_dir = {1.0f, 0.0f, 0.3f};
    // 波浪相位时间（秒）。确定性：由帧号推，便于截图复现。
    float wave_time = 0.0f;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_WATER_TYPES_H_

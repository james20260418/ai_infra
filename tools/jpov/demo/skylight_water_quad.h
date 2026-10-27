// JPOV 天光查看器 —— 水面场景的几何与材质参数（GL-free，header-only）
//
// 这一步的目标（2026-10-27 Danis：小步前进）：
//   先把「水皮」当成**一块巨大的水平半透明玻璃板**验收 —— 不接任何水体吸收/散射、
//   不接反射探针，只看两件事：
//     ① 一块 10 km² 的平面在这个视角下能不能被条带正常画出来（透视、深度、远裁剪）；
//     ② 玻璃材质（覆盖集 + 菲涅耳边反）在水面这个**几乎全身掠射**的几何上是什么观感。
//
// 为什么用条带（Strip3D）而不是一个普通 quad：
//   引擎里**半透明只有 Strip3D 一条路径** —— 透明的 object3d 玻璃 pass 已整体移除
//   （见 interface/pbr_material.h 的 AlphaMode 注释）。水皮要「半透明 + 边反」，
//   只能走条带。这也正是需求里说的「用 strip 平铺一个 quad」。
//
// 尺寸与切法：
//   面积 kWaterQuadAreaKm2 = 10 km²（正方形 ⇒ 边长 √1e7 ≈ 3162.3 m），
//   沿 Z 切成 kWaterQuadTileRows 条，每条是一条沿 X 走的 Strip32（32 个采样点）。
//   16 条 / 一次 draw call。切行数只影响「一条带多宽」，不影响观感（平面无曲率），
//   但**必须 > 1** —— 需求要的是「平铺」而不是单条超宽条带。
//
// 尺度选择的理由：人眼高 1.7 m 时几何地平线在 ~4.6 km。3.16 km 见方 ⇒ 站在水边
// 能一直看到这块面的**远端边界**，于是「近处看得穿、远处 Fresnel 发白」这条梯度
// 在屏幕上是连续可见的；远裁剪面 kWaterSceneFarM 必须盖住对角（~4.5 km）与天空。

#ifndef JPOV_DEMO_SKYLIGHT_WATER_QUAD_H_
#define JPOV_DEMO_SKYLIGHT_WATER_QUAD_H_

#include <cmath>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/strip_types.h"

namespace jpov_skylight {

// 水面面积（平方千米）。改它 = 改整块水面的尺寸（边长随之由 WaterQuadSideM 推导）。
inline constexpr float kWaterQuadAreaKm2 = 10.0f;

// 沿 Z 方向把这块面切成几条（每条 = 一条 Strip32）。必须 >= 1。
inline constexpr int kWaterQuadTileRows = 16;

// 水面所在的世界高度（米，y-up）。海底/水下几何将来从这里往下接。
inline constexpr float kWaterPlaneY = 0.0f;

// 水面场景的远裁剪面（米）。
//   10 km² 正方形的对角 ≈ 4.47 km，加上天空/大气背景，取 20 km 足够有余。
//   模型场景仍用原来的 1000 m（零回归）—— 见 skylight_viewer_app.h 的 OneIteration。
inline constexpr float kWaterSceneFarM = 20000.0f;

// 正方形水面的边长（米）= √(面积)。
inline float WaterQuadSideM() {
    return std::sqrt(kWaterQuadAreaKm2 * 1.0e6f);   // km² → m²
}

// 把 side_m × side_m 的水平四边形沿 Z 切成 tile_rows 条，条带沿 X 方向走。
//
// 每条带的两个轨（right / left）分别落在该行的 z0 / z0+行宽 上；
// 32 个采样点均匀分布在 [-side/2, +side/2] 的 X 区间里（平面无曲率，
// 采样密度只影响顶点数，不影响形状）。
//
// Pre-condition: side_m > 0、tile_rows >= 1（否则 CHECK 崩）。
inline std::vector<jpov::Strip32> BuildWaterQuadStrips(float side_m, int tile_rows) {
    CHECK_GT(side_m, 0.0f) << "BuildWaterQuadStrips: side_m 必须 > 0";
    CHECK_GE(tile_rows, 1) << "BuildWaterQuadStrips: tile_rows 必须 >= 1";

    constexpr int kColumns = jpov::kStrip32SampleCount;   // 32 个采样点
    const float half = 0.5f * side_m;
    const float row_depth = side_m / static_cast<float>(tile_rows);

    std::vector<jpov::Strip32> strips;
    strips.reserve(static_cast<size_t>(tile_rows));
    for (int r = 0; r < tile_rows; ++r) {
        const float z_right = -half + row_depth * static_cast<float>(r);   // v = 0
        const float z_left  = z_right + row_depth;                         // v = 1
        jpov::Strip32 strip;
        for (int i = 0; i < kColumns; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kColumns - 1);
            const float x = -half + t * side_m;
            strip.right[i] = jpov::Vec3f(x, kWaterPlaneY, z_right);
            strip.left[i]  = jpov::Vec3f(x, kWaterPlaneY, z_left);
        }
        strip.point_count = static_cast<uint32_t>(kColumns);
        strips.push_back(strip);
    }
    return strips;
}

// 玻璃（水皮）的表面色与不透明度。
//   rgb   = 迎面看到的水色（偏青的冷色，线性 HDR）；
//   alpha = **正对视线**时的覆盖率（0.10 ⇒ 一眼看穿，几乎只是一层很淡的色偏）。
//   远处/掠射处的实心与亮边不靠它，靠 fresnel 的 graze_alpha + rim_color。
inline jpov::Color WaterGlassBodyColor() {
    return jpov::Color{0.62f, 0.80f, 0.86f, 0.10f};
}

// 玻璃（水皮）的边反参数（见 interface/strip_types.h:StripFresnel）。
//   f0 = 0.02 是水的正入射反射率（玻璃 0.04）；power 5 = 物理 Schlick 指数；
//   graze_alpha = 1 ⇒ 掠射处完全不透明（远处水面收敛成一条亮带，这正是要看的观感）；
//   rim_color 偏冷 —— 水面反射的是天光，不是中性的白。
inline jpov::StripFresnel WaterGlassFresnel() {
    jpov::StripFresnel f;
    f.f0 = 0.02f;
    f.power = 5.0f;
    f.strength = 1.0f;
    f.graze_alpha = 1.0f;
    f.rim_color = jpov::Vec3f{0.80f, 0.90f, 1.00f};
    return f;
}

// 整块 10 km² 水面（一次 draw call）：
//   覆盖集（吸收/遮挡介质，玻璃的正确集合）+ 折线几何（平面没有曲率，不需要 kSmooth）
//   + 菲涅耳边反（玻璃的观感来源）。
inline jpov::TranslucentStrip32Batch MakeWaterQuadBatch() {
    jpov::TranslucentStrip32Batch batch;
    batch.strips = BuildWaterQuadStrips(WaterQuadSideM(), kWaterQuadTileRows);
    batch.blend = jpov::StripBlend::kCoverage;
    batch.geometry = jpov::StripInterpolation::kPolyline;
    batch.fresnel = WaterGlassFresnel();

    const jpov::Color body = WaterGlassBodyColor();
    for (jpov::Strip32& strip : batch.strips) {
        strip.point_color.fill(body);
    }
    return batch;
}

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_WATER_QUAD_H_

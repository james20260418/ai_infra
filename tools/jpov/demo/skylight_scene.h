// JPOV 天光查看器（skylight viewer）— 场景 + 光照装配（header-only）
//
// 目的（Danis 需求）：一个专门用来**肉眼验收天光（SkyCommand）**的最小场景——
// 固定三个不同 PBR 材质的方块，配上可调的太阳仰角 / 浊度 / 季节 / 天光强度，
// 一直把太阳压到 0°（落山）就能看到纯夜色（night_zenith_color /
// night_horizon_color 的加法叠加效果）。
//
// 为什么用三个方块而不是加载 glTF（与 model viewer 的区别）：
//   - model viewer 的目的是"看模型"，天光只是背景；本查看器的目的是"看天光"，
//     被测物要**材质可控**且**常量**，否则每换一个模型就换一组变量，无法标定。
//   - 三块方块的材质刻意隔离变量（Danis 定的三段）：
//       低反（rough=1.0, metal=0）→ 纯漫反射，最能反映"天空/环境光的平均色与量"；
//       高光（rough=0.05, metal=0, 同 albedo）→ 只差粗糙度，用于看太阳/月色高光
//         的**颜色与位置**（低反块看不见高光，两者对比即"高光从哪来"）；
//       金属（metal=1, rough=0.15）→ 无漫反射，只反射环境 → 最能反映"天光的
//          低频分布"（夜里金属块会显得比漫反射块还暗或还亮，暴露 ambient 是否合理）。
//     三块 albedo 统一（0.55 中性灰），把"材质"这一个变量压到只剩 metallic/rough。
//
// 文件归属：纯场景/光照装配（header-only，无 GL 状态），可被 demo 主程序与将来
// 的 headless 拍摄复用；交互面板在 viewer_app.h 的派生类里。

#ifndef JPOV_DEMO_SKYLIGHT_SCENE_H_
#define JPOV_DEMO_SKYLIGHT_SCENE_H_

#include <cmath>

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_skylight {

// ── "经典城市夜色"锚点（线性 HDR，感知标尺）──
//
// 夜里无月、城郊/城市光污染的典型天色：天顶最深（偏冷蓝紫），地平线明显更亮
// 且偏暖（钠灯/城市光污染在大气中散射出的橙黄晕）。这两个端点是本查看器的
// **固定常量**（不是滑条）——Danis 的验收对象是"夜色这两个颜色对不对"，
// 所以先把观感锚在一组经典值上，再用"天光强度"滑条调整体亮度。
//
// 量级说明（重要）：这是**感知标尺**而非物理标尺。严格物理的夜/日比 ≈ 2.5e-6
// （满月地面 ≈0.25 lux vs 晴天 ≈1e5 lux），在 ACES tone map 下等于全黑、肉眼
// 无法验收。本组值经 ACES + sRGB 输出后，天顶 ≈ 12/255、地平 ≈ 60/255——
// "看得出是夜景，但明确是夜"。推导与调参纪律见 interface/LIGHT_INTENSITY.md 第十节。
inline constexpr jpov::Color kCityNightZenith  = {0.010f, 0.013f, 0.024f, 1.0f};
inline constexpr jpov::Color kCityNightHorizon = {0.055f, 0.048f, 0.045f, 1.0f};

// ── 三方块在场景中的布局 ──
//
// 沿 +X 一字排开（间距 2×half），中心在 y = 0（方块底面贴 y=0 地面）。
// 三个方块尺寸相同（1×1×1），只材质不同——把"位置/大小"这个变量也压掉，
// 剩下的差异全部来自材质。
inline constexpr float kBoxHalf    = 0.5f;   // 半宽（1×1×1 方块）
inline constexpr float kBoxSpacing = 1.6f;   // 相邻方块中心距（米）

// 三块方块的材质（Danis 定的三段）。albedo 统一 0.55 中性灰。
//
//   index 0：低反（粗糙漫反射）——roughness=1.0 且 metallic=0
//   index 1：高光（光滑非金属）——roughness=0.05 且 metallic=0（同 albedo，隔离变量）
//   index 2：金属              ——metallic=1.0 且 roughness=0.15（无漫反射，纯环境反射）
inline jpov::PBRMaterial MaterialLowGloss() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.55f, 0.55f, 0.55f, 1.0f},
        /*metallic*/ 0.0f,
        /*roughness*/ 1.0f);
}
inline jpov::PBRMaterial MaterialHighGloss() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.55f, 0.55f, 0.55f, 1.0f},
        /*metallic*/ 0.0f,
        /*roughness*/ 0.05f);
}
inline jpov::PBRMaterial MaterialMetal() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.55f, 0.55f, 0.55f, 1.0f},
        /*metallic*/ 1.0f,
        /*roughness*/ 0.15f);
}

// 方块 i（0/1/2）在场景中的世界中心：沿 +X 对称排开，y 抬到方块中心高度。
inline jpov::Vec3f BoxCenter(int index) {
    const float x = (static_cast<float>(index) - 1.0f) * kBoxSpacing;  // -1.6, 0, +1.6
    return {x, kBoxHalf, 0.0f};   // 底面贴地：中心 y = 半宽
}

// ── 地面：中性灰、高粗糙（与 model viewer 同款语义，但更小更贴场景）──
//
// 300×300 的大地在这里没有意义（本场景只关心方块上的天光反射），改用 40×40 的
// 中等地面：够接住方块的接触阴影，又不会把画面稀释成一片灰。灰值 0.35 略暗于
// 方块，便于肉眼区分"地面上收到的天光"与"方块面收到的天光"。
inline constexpr float kGroundY     = 0.0f;    // 地面高度（方块底面所在平面）
inline constexpr float kGroundHalf  = 20.0f;   // 40×40 米

inline jpov::MeshData MakeGroundQuad() {
    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
    const float y = kGroundY;
    const float h = kGroundHalf;
    mesh.positions = {
        {-h, y,  h},  // 0
        { h, y,  h},  // 1
        { h, y, -h},  // 2
        {-h, y, -h},  // 3
    };
    mesh.normals = {
        {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.Validate();
    return mesh;
}

inline jpov::PBRMaterial GroundMaterial() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.35f, 0.35f, 0.35f, 1.0f},
        /*metallic*/ 0.0f,
        /*roughness*/ 1.0f);
}

// ── 天光装配：由滑条状态构造一整组光照（sky + sun + ambient）──
//
// 这是本查看器与 model viewer 的核心差别所在，故单列并写清语义：
//
// 参数：
//   elev_deg     — 太阳仰角（度，[0,90]）。0° = 贴地日落 → 白天项被 shader 的
//                  daylight 因子精确压到 0，画面**只剩夜色**（本查看器的主要用途）。
//                  90° = 正午。
//   turbidity    — 大气浊度 [2,8]。影响 (a) 天空散射色/日盘 + (b) sun/ambient
//                  的浊度衰减乘子（TurbSunLoss/TurbAmbLoss）+ (c) **月晕宽度**
//                  （浊度越大晕越宽，见 sky_renderer.h 的 discGlow 调用）。
//   season_r     — 季节 R 色温乘子 [0.5,2.0]。**只染白天项**（见 SkyCommand 注释：
//                  season 是"日光散射的季节色温"）——夜色项不受它影响，用滑条
//                  拉到极端即可肉眼验证"夜色不被季节染色"。
//   night_scale  — 夜色强度乘子 [0,4]，默认 1.0。它**只乘夜色两色**，不乘白天项，
//                  方便在太阳未落时单独观察夜色分量（=0 时完全关闭夜色，用于
//                  对照"没有夜色的同一画面"）。
//                  实现方式：把 kCityNight* × night_scale 直接写进 sky 的两个
//                  night 颜色；sky.intensity 保持 1.0 不动（intensity 是"天光总
//                  开关"，同时作用于日夜两层，不适合拿来做夜色的独立旋钮）。
//   moon_elev_deg   — 月亮仰角（度，[0,90]），与太阳仰角**独立**（日月的场景是
//                  两个天体各自的位置）；月盘方向 = 同方位 + 极角，但**水平方位
//                  与太阳相反**（本查看器的摆放选择：日月各在天球一侧，见下）。
//   moon_brightness — 月盘自发光亮度基数（绝对量；0 = 不画月盘）。
//   moon_glow       — 月晕强度（绝对量；0 = 无月晕）。
//
// 注：sun/ambient 的强度不在此函数参数里——本函数只造 sky；平行光与环境光由
//     MakeSun()/MakeAmbient() 另造（它们只取 sky 的色调，强度由调用方给绝对量，
//     见下）。
inline jpov::SkyCommand MakeSky(float elev_deg, float turbidity, float season_r,
                                float night_scale, float moon_elev_deg,
                                float moon_brightness, float moon_glow) {
    const float deg2rad = 3.14159265358979323846f / 180.0f;
    const float elev_rad = elev_deg * deg2rad;
    const float sy = std::sin(elev_rad);
    const float sx = std::cos(elev_rad);
    const jpov::Vec3f sun_dir = {sx, sy, 0.0f};   // 指向太阳（+X 侧升起）

    // 月盘方向：**水平方位与太阳相反**（−X 侧升起），仰角独立由 moon_elev_deg 给。
    // 这是**查看器的摆放选择**（日月各在天球一侧，便于同屏对照），不是接口契约——
    // sky.moon_dir 是独立输入，接口不假设"月 = −日"。
    const float moon_rad = moon_elev_deg * deg2rad;
    const float mx = -std::cos(moon_rad);
    const float my = std::sin(moon_rad);

    jpov::SkyCommand sky{};
    sky.sun_dir     = sun_dir;
    sky.moon_dir    = {mx, my, 0.0f};
    sky.turbidity   = turbidity;
    sky.season      = {season_r, 1.0f, 1.0f, 1.0f};   // 只调 R 通道（季节色温）
    sky.intensity   = 1.0f;                            // 天光总开关（日夜共用）
    sky.ground_color = {0.02f, 0.02f, 0.025f, 1.0f};   // 夜间地色偏暗（地平线下）
    sky.sun_radius     = 0.02f;
    sky.sun_brightness = 1e3f;
    sky.sun_glow       = 0.0f;   // 日盘不开光晕（本查看器只看盘与天色）
    // 月盘：半径与日盘同值；亮度/光晕由滑条给绝对量（0 = 不画）。
    sky.moon_radius     = 0.02f;
    sky.moon_brightness = moon_brightness;
    sky.moon_glow       = moon_glow;
    // 夜色两色 × 夜色强度（滑条独立旋钮；=0 即关闭夜色，用于对照）。
    sky.night_zenith_color  = {kCityNightZenith.r  * night_scale,
                               kCityNightZenith.g  * night_scale,
                               kCityNightZenith.b  * night_scale,
                               1.0f};
    sky.night_horizon_color = {kCityNightHorizon.r * night_scale,
                               kCityNightHorizon.g * night_scale,
                               kCityNightHorizon.b * night_scale,
                               1.0f};
    return sky;
}

// 由 sky 推导平行光（方向 = 反太阳方向；色调由 sky 给，亮度由调用方给）。
// intensity 是**绝对强度**（滑条所见即所得），不乘 PWL 相对衰减曲线——本查看器
// 的用途是标定"某个仰角下太阳该多亮"，需要绝对可控；需要"由 sky 自动推导"的行为
// 请用 model viewer（MakeLighting）。色调仍由 sky 自动推导（色调的物理性交给 sky，
// 亮度的手感交给滑条）。
inline jpov::DirectionalLight MakeSun(const jpov::SkyCommand& sky,
                                      float intensity) {
    return jpov::DirectionalLight{
        /*direction*/ {-sky.sun_dir.x(), -sky.sun_dir.y(), -sky.sun_dir.z()},
        /*color*/ sky.DirectionalColor(),
        /*intensity*/ intensity,
    };
}

// 由 sky 推导环境光（色调由 sky 给，亮度由调用方给）。intensity 同为绝对量（见上）。
inline jpov::AmbientLight MakeAmbient(const jpov::SkyCommand& sky,
                                      float intensity) {
    return jpov::AmbientLight{
        .color = sky.AmbientColor(),
        .intensity = intensity,
    };
}

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_SCENE_H_

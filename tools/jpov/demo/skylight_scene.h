// JPOV 天光查看器（skylight viewer）— 场景 + 天光装配（header-only）
//
// 目的：一个专门用来**肉眼验收天光（SkyCommand）**的最小场景——
// 固定三个不同 PBR 材质的方块 + 灰色地面，配上三个自由度（浊度 / 季节色温 /
// 天体方向），一眼看全「标准天光」在材质上的反映。
//
// ⭐ 天光一律由 `CreateDefaultSkyCommand` 构造（其余参数走默认构造值），
//    画面光照（主平行光 + ambient）则由该 SkyCommand **推导**——保证「看到的
//    天色」与「物体受光」同源，不会各配一套而漂移。天体方向只给一条仰角轴：
//    月亮恒在反日点（moon_dir = −sun_dir），据此白天用太阳主光、夜间用月亮主光。
//
// 为什么用三个方块而不是加载 glTF（与 model viewer 的区别）：
//   - model viewer 的目的是"看模型"，天光只是背景；本查看器的目的是"看天光"，
//     被测物要**材质可控**且**常量**，否则每换一个模型就换一组变量，无法标定。
//   - 三块方块的材质刻意隔离变量（三段）：
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

// ── 三方块在场景中的布局 ──
//
// 沿 +X 一字排开（间距 2×half），中心在 y = 0（方块底面贴 y=0 地面）。
// 三个方块尺寸相同（1×1×1），只材质不同——把"位置/大小"这个变量也压掉，
// 剩下的差异全部来自材质。
inline constexpr float kBoxHalf    = 0.5f;   // 半宽（1×1×1 方块）
inline constexpr float kBoxSpacing = 1.6f;   // 相邻方块中心距（米）

// 三块方块的材质（三段）。albedo 统一 0.55 中性灰。
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

// ── 自由度的取值范围与编码（面板与拍摄共用，单一出处）──
//
// turbidity：大气浊度 [0, 8]，默认 2（清澈）。
// 季节色温：daylight_season 乘子（Color，**只染太阳能通道**）。查看器把它压缩成
//     **一个滑条**：左端 = 蓝偏、右端 = 红偏、中间 = 中性；G 通道恒 1。
//     这是**查看器的交互设计**，不是 SkyCommand 的接口契约（接口收的是 Color）。
// 月色变红：moon_season 乘子（Color，**只染月盘 + 月光**）。查看器一个滑条 [0,1]：
//     0 = 中性常月，1 = 血月（又红又暗）。不走亮度归一化（有意让它变暗）。
// 夜蓝：夜色两色（night_zenith/horizon_color）的整体乘子（Color）。查看器一个滑条
//     [0,1]：0 = 出厂夜色，1 = 梦幻蓝且更亮。**不改接口**，只是把出厂两色乘一下。
// 天体方向：世界空间 y-up 方向，按**仰角(度) + 方位角(度)** 两个滑条给。
//     仰角 [−90, +90]（0=地平线；正=太阳在地平线上，负=太阳沉下、月亮升到反向等高）；
//     方位角 [0,360) 绕 +Y。月亮方向恒取 −sun_dir，不单独占滑条。
inline constexpr float kTurbidityMin   = 0.0f;
inline constexpr float kTurbidityMax   = 8.0f;
inline constexpr float kTurbidityDef   = 2.0f;

// 季节色温偏置幅度：滑条端点 ±kSeasonTintSpan（乘到 R / 除到 B）。
//   红偏端 daylight_season=(1+span, 1, 1-span)；蓝偏端 daylight_season=(1-span, 1, 1+span)。
inline constexpr float kSeasonTintSpan = 0.25f;

// 把 [-1,+1] 的季节滑条值编码成 daylight_season（-1=蓝偏，0=中性，+1=红偏）。
//   R = 1 + span·t，B = 1 − span·t，G = 1。
// DaylightSeasonTintScale() 会把总亮度归一回去（只偏色温、不增减光强）。
inline jpov::Color DaylightSeasonTintFromSlider(float t) {
    const float tc = std::max(-1.0f, std::min(1.0f, t));
    return {1.0f + kSeasonTintSpan * tc, 1.0f, 1.0f - kSeasonTintSpan * tc, 1.0f};
}

// 把 [0,1] 的「月色变红」滑条值编码成 moon_season（0=中性常月，1=血月）。
// 压绿 0.80、压蓝 0.97（R 不动）⇒ 血色；**不归一化** ⇒ 总亮度随滑条下降，血月又红又暗。
inline jpov::Color MoonSeasonFromSlider(float t) {
    const float tc = std::max(0.0f, std::min(1.0f, t));
    return {1.0f, 1.0f - 0.80f * tc, 1.0f - 0.97f * tc, 1.0f};
}

// 把 [0,1] 的「夜蓝」滑条值编码成夜色两色的整体乘子（0=出厂，1=最蓝最亮）。
// 压红 0.45、提绿 0.25、提蓝 2.40 ⇒ 色相偏蓝且亮度上升（蓝通道 + 绿通道一起抬，
// 保证 ACES 后人眼亮度（G 主）也真的变亮，而不是“只是变蓝”）。“梦幻蓝、有点亮”。
inline jpov::Color NightTintFromSlider(float t) {
    const float tc = std::max(0.0f, std::min(1.0f, t));
    return {1.0f - 0.45f * tc, 1.0f + 0.25f * tc, 1.0f + 2.40f * tc, 1.0f};
}

// 由「仰角(度) + 方位角(度)」构造方向单位向量（y-up）。
//   仰角 0=地平线，90=天顶；方位角绕 +Y，0=+Z，90=+X。
inline jpov::Vec3f DirFromAngles(float elev_deg, float azim_deg) {
    const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    const float e = elev_deg * kDeg2Rad;
    const float a = azim_deg * kDeg2Rad;
    const float ce = std::cos(e);
    return {ce * std::sin(a), std::sin(e), ce * std::cos(a)};
}

// ── 天光装配：由查看器自由度构造 SkyCommand，并**由它推导**全部光照 ──
//
// 场景另加两个模型（桌子 / 高模橡树）供夜色标定时观察物体受光；模型加载与
// 摆放见 LoadSceneModels()（资源由 build 脚本拷到 exe 旁 models/，相对路径硬编码）。
//
// 这是本查看器与 model viewer 的核心差别所在，故单列并写清语义：
//
//   sky = CreateDefaultSkyCommand(turb, daylight_season, sun_dir, moon_dir)
//         —— 其余参数（日/月盘半径与亮度、夜色两色、ground_color、intensity…）
//            一律走 SkyCommand 的默认构造值（= 标准天光）；
//            再用两个滑条覆盖 `moon_season` 与夜色两色（月色/夜蓝），见 SkyDegrees。
//
//   月亮方向恒取 **moon_dir = −sun_dir**（满月落在反日点）：查看器只给一条
//   「仰角」滑条 [−90°, +90°]，正=太阳在地平线上、负=太阳沉下（月亮升到反向等高）。
//   同一条仰角轴于是覆盖「日光 / 月光」两种主光，切换面在 sun_dir.y = 0：
//
//     sun_dir.y >= 0（白天）：主光 = 太阳平行光
//       direction = −sun_dir （光传播方向，从上往下）
//       color     = sky.SunDirectionalColor()   intensity = sky.SunDirectionalIntensity()
//     sun_dir.y < 0（夜间）：主光 = 月亮平行光
//       direction = −moon_dir = +sun_dir （月亮在天上，光同样从上往下）
//       color     = sky.MoonDirectionalColor()  intensity = sky.MoonDirectionalIntensity()
//
//   ambient（环境光）始终由 sky 推导，与天色/昼夜同源：
//     ambient.color     = sky.AmbientColor()     （含夜色叠加；夜色端不吃 daylight_season）
//     ambient.intensity = sky.AmbientIntensity() （含夜间项）
//
// 返回：装配好的完整光照三元组。
struct SkyLighting {
    jpov::SkyCommand sky;
    // 主平行光：白天是太阳、夜间是月亮（按 sun_dir.y 切换，见上）。渲染侧只有一个
    // 平行光槽（RenderCommandList::sun），故日/月共用这一个字段。
    jpov::DirectionalLight dir_light;
    jpov::AmbientLight ambient;
};

// 查看器的天光自由度（滑条 → SkyCommand 字段的单点编码，面板与拍摄共用）。
struct SkyDegrees {
    float turbidity = kTurbidityDef;   // ① 浊度 [0,8]
    float daylight_season = 0.0f;      // ② 季节色温 [−1,+1] → daylight_season
    float sun_elev_deg = 45.0f;        // ③ 天体仰角 [−90,+90]
    float sun_azim_deg = 45.0f;        // ③ 天体方位角 [0,360)
    float moon_red = 0.0f;             // ④ 月色变红 [0,1] → moon_season（1=血月）
    float night_blue = 0.0f;           // ⑤ 夜蓝 [0,1] → 夜色两色整体偏蓝（1 最蓝最亮）
};

inline SkyLighting MakeSkyLighting(const SkyDegrees& d) {
    const jpov::Vec3f sun_dir = DirFromAngles(d.sun_elev_deg, d.sun_azim_deg);
    // 满月落在反日点：moon_dir = −sun_dir（月盘位置与光源方位由此单点确定）。
    const jpov::Vec3f moon_dir = {-sun_dir.x(), -sun_dir.y(), -sun_dir.z()};
    jpov::SkyCommand sky = jpov::CreateDefaultSkyCommand(
        d.turbidity, DaylightSeasonTintFromSlider(d.daylight_season), sun_dir,
        moon_dir);
    // 月色（血月）与夜蓝：覆盖标准天光对应的两个字段（其余仍走默认）。
    sky.moon_season = MoonSeasonFromSlider(d.moon_red);
    const jpov::Color nt = NightTintFromSlider(d.night_blue);
    sky.night_zenith_color =
        jpov::Color{sky.night_zenith_color.r * nt.r,
                    sky.night_zenith_color.g * nt.g,
                    sky.night_zenith_color.b * nt.b, 1.0f};
    sky.night_horizon_color =
        jpov::Color{sky.night_horizon_color.r * nt.r,
                    sky.night_horizon_color.g * nt.g,
                    sky.night_horizon_color.b * nt.b, 1.0f};

    SkyLighting out;
    if (sun_dir.y() >= 0.0f) {
        // 白天：太阳平行光（传播方向 = −sun_dir）。
        out.dir_light = jpov::DirectionalLight{
            /*direction*/ {-sun_dir.x(), -sun_dir.y(), -sun_dir.z()},
            /*color*/ sky.SunDirectionalColor(),
            /*intensity*/ sky.SunDirectionalIntensity(),
        };
    } else {
        // 夜间：月亮平行光（传播方向 = −moon_dir = +sun_dir）。
        out.dir_light = jpov::DirectionalLight{
            /*direction*/ sun_dir,
            /*color*/ sky.MoonDirectionalColor(),
            /*intensity*/ sky.MoonDirectionalIntensity(),
        };
    }
    out.ambient = jpov::AmbientLight{
        .color = sky.AmbientColor(),
        .intensity = sky.AmbientIntensity(),
    };
    out.sky = sky;
    return out;
}

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_SCENE_H_

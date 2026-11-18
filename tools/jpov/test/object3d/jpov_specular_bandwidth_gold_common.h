// JPOV Gold — 镜面高光的「带宽」（光源角半径 θ）
//
// 背景（docs/jpov_specular_bandwidth.md）：完美镜面 + 平行光 = 两个 δ 相乘，点值无穷、只能积分。
//   逐像素采样拿到 ∞ 或 0 ⇒ 高光要么爆掉、要么彻底消失。带宽的物理下限就是太阳的
//   角半径（θ ≈ 0.004654 rad ≈ 0.53° 直径）。本测试把这个问题**做成一张看得懂的图**：
//   场景就是 skylight viewer 的镜面地板场景（同一份 App / 同一个场景与机位函数，
//   `InstallMirrorFloorScene()` + `SetMirrorSpecularView()`），每张只在
//   roughness 与 **太阳角半径 θ** 上不同。
//
// 为什么用 viewer 的场景而不是一块孤零零的深色 quad：gold 图是给人看的。
//   「渐变天光 + 200×200 m 大镜面地面 + 散布方块」这张图，一眼就能看出
//   ①这是镜子（映着天光和方块）②太阳的像在不在。孤立的 quad 图只有一片黑，读不出信息。
//
// 机位**算死**，不靠肉眼对齐（推导见 skylight_viewer_app.h）：地面 y=0，相机
//   (R cosφ sinθ, R sinφ, R cosφ cosθ) 看向原点 ⇒ 原点处的反射方向 = 仰角 φ、方位 θ+180。
//   取 φ = 太阳仰角、θ = 太阳方位 − 180° ⇒ **太阳的像精确落在画面正中**，于是：
//     · 图里能直接比较「天上那颗日盘」与「地上它的像」的大小；
//     · 测试可以按像素位置断言，不需要模板匹配。
//
// 两种失败形态（同一根因：没有带宽）与它们的对照：
//   ① r=0（纯 δ）：a2 = roughness⁴ = 0 ⇒ D ≡ 0 ⇒ 太阳的像**完全不存在**
//      （修之前实测：整幅地面一个比底色亮的像素都没有）；
//   ② r 很小但非零（如 0.02）：lobe 宽度远小于一个像素 ⇒ 峰值被 `max(denom,1e-7)`
//      护栏钳住 ⇒ 越光滑反而越暗、能量不守恒。
//   接上 θ 之后两者都变成「看得见的日盘」。
//
// 文件归属：本头文件被 generator（写仓库 gold PNG）与 test（渲染 + 定量断言）共用，
//   保证两边场景完全一致；场景本身又来自 demo，不会与 viewer 分叉。

#ifndef JPOV_TEST_OBJECT3D_JPOV_SPECULAR_BANDWIDTH_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_SPECULAR_BANDWIDTH_GOLD_COMMON_H_

#include <string>

#include <glog/logging.h>

#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_specular_bandwidth {

// 画幅（两张图同尺寸，test 里按像素统计）。
inline constexpr int kImageWidth = 1280;
inline constexpr int kImageHeight = 720;

// 「等效 δ」的角半径：比像素尺寸小 5 个量级 ⇒ 与「没有带宽」在实际采样上等价，
// 也正是修之前 shader 完全忽略 θ 时的行为。不是调试开关，就是光源的物理参数。
inline constexpr float kNoBandwidthAngularRadius = 1.0e-6f;

// 太阳的像落在画面正中（见文件头推导），测试用它做位置断言。
inline constexpr int kExpectedCenterX = kImageWidth / 2;
inline constexpr int kExpectedCenterY = kImageHeight / 2;

// 一张「拍摄」= 一组对照条件。
struct Shot {
    float roughness = 0.0f;          // 地面材质 roughness
    bool env_reflections = false;    // true = 开 env/SSR（镜子观感）；false = 只留太阳的镜面项
    float sun_angular_radius = 0.0f; // 0 = 用默认（太阳 0.004654）；1e-6 = 等效 δ
    const char* file_tag = "";       // 进文件名
};

inline constexpr Shot kShots[] = {
    // ① 钱图：只有太阳的镜面项 ⇒ 除日盘的像外地面纯黑。修之前这张是**一片全黑**。
    {0.0f,  false, 0.0f,                  "r0.000_pure"},
    // ② 负向对照：同场景、同太阳，只把 θ 缩到 1e-6 ⇒ 日盘的像必须消失
    //    （= 修之前那一版的行为）。
    {0.0f,  false, kNoBandwidthAngularRadius, "r0.000_pure_no_bandwidth"},
    // ③ 正常镜子观感：地面映天光与方块，日盘的像叠在上面。
    {0.0f,  true,  0.0f,                  "r0.000_mirror"},
    // ④ 同上，θ → 等效 δ：镜子里没有日盘（差别只在画面正中那条窄带）。
    {0.0f,  true,  kNoBandwidthAngularRadius, "r0.000_mirror_no_bandwidth"},
    // ⑤ θ 主导：r=0.02 的 a2 只有 θ 带宽的 3% ⇒ 应与 ① 几乎同一张图。
    {0.02f, false, 0.0f,                  "r0.020_pure"},
    // ⑥ 粗糙端由 roughness 主导：不能被这次改动弄没（本该有光）。
    {0.30f, true,  0.0f,                  "r0.300_mirror"},
};
inline constexpr int kShotCount = 6;

inline std::string ShotFileName(const Shot& shot) {
    return std::string("specular_bw_") + shot.file_tag + "_1280x720.png";
}

// 把 app 配成这一张拍摄的全部条件（generator 与 test 共用，不会两边各写一套）。
// Pre-condition: app 已 Init() 且已调用 InstallMirrorFloorScene()/SetMirrorSpecularView()。
inline void ApplyShot(jpov_skylight::SkylightApp* app, const Shot& shot) {
    CHECK(app != nullptr);

    app->SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(shot.roughness));

    if (shot.env_reflections) {
        // 天光消费量 0.35（不是 1.0）：镜面在「太阳的像」那个方向映的正是日盘周围最亮
        // 的天光，全天光会把日盘一起过曝成白、对比度全丢（见 MirrorFloorMaterial 注释）。
        app->SetEnvSpecular(true, 0.35f);
        app->SetSsr(true);
        app->ClearAmbientOverride();
    } else {
        // 把太阳的镜面项完全隔离出来：关 env/SSR + 环境光置零。
        // （要关环境光是因为 metallic 目前仍吃到 diffuse ambient 那一项 —— 那是另一件事。）
        app->SetEnvSpecular(false);
        app->SetSsr(false);
        jpov::AmbientLight no_ambient{};
        no_ambient.intensity = 0.0f;
        app->SetAmbientOverride(no_ambient);
    }

    if (shot.sun_angular_radius > 0.0f) {
        // 只换角半径：方向/颜色/强度全取当前天光推导值（不手搓一份光，免得与 sky 漂移）。
        jpov::DirectionalLight sun =
            jpov_skylight::MakeSkyLighting(app->deg_, /*tricolor_ambient=*/false).dir_light;
        sun.angular_radius = shot.sun_angular_radius;
        app->SetSunOverride(sun);
    } else {
        app->ClearSunOverride();
    }
}

}  // namespace jpov_specular_bandwidth

#endif  // JPOV_TEST_OBJECT3D_JPOV_SPECULAR_BANDWIDTH_GOLD_COMMON_H_

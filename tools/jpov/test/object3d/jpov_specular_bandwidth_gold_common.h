// JPOV Gold — 镜面高光的「带宽」（光源角半径 θ）
//
// 问题（docs/jpov_specular_bandwidth.md）：完美镜面 + 平行光 = 两个 δ 相乘，逐像素采样
//   只能拿到 ∞ 或 0 ⇒ 高光要么爆掉、要么彻底消失。带宽的物理下限就是太阳的角半径
//   （θ ≈ 0.004654 rad ≈ 0.53° 直径）。修好之后 `a2_eff = roughness⁴ + (θ/2)²`，
//   于是完美镜面（roughness=0）的地面上也会出现**太阳的像**。
//
// 本 gold 只锁**修好之后**的行为（不留「把 θ 关掉」的对照图 —— 引擎里也没有这种开关：
//   `DirectionalLight::angular_radius` 填 0 会被 renderer CHECK 打回）。
//
// 场景就是 skylight viewer 的镜面地板场景（同一份 App / 同一个场景与机位函数，
//   `InstallMirrorFloorScene()` + `SetMirrorSpecularView()`），每张只在 roughness 与
//   是否开天光反射上不同。为什么用 viewer 的场景：gold 图是给人看的 ——
//   「渐变天光 + 200×200 m 大镜面地面 + 散布方块」一眼就能看出①这是镜子②太阳的像在不在，
//   而孤立的深色 quad 只有一片黑，读不出信息。
//
// 机位**算死**，不靠肉眼对齐（推导见 skylight_viewer_app.h）：地面 y=0，相机
//   (R cosφ sinθ, R sinφ, R cosφ cosθ) 看向原点 ⇒ 原点处的反射方向 = 仰角 φ、方位 θ+180。
//   取 φ = 太阳仰角、θ = 太阳方位 − 180° ⇒ **太阳的像精确落在画面正中**，于是：
//     · 图里能直接比较「天上那颗日盘」与「地上它的像」的大小；
//     · 测试可以按像素位置断言，不需要模板匹配。
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

// 太阳的像落在画面正中（见文件头推导），测试用它做位置断言。
inline constexpr int kExpectedCenterX = kImageWidth / 2;
inline constexpr int kExpectedCenterY = kImageHeight / 2;

// 一张「拍摄」= 一组对照条件。
struct Shot {
    float roughness = 0.0f;        // 地面材质 roughness
    bool env_reflections = false;  // true = 开 env/SSR（镜子观感）；false = 只留太阳的镜面项
    const char* file_tag = "";     // 进文件名
};

inline constexpr Shot kShots[] = {
    // ① 钱图：只有太阳的镜面项 ⇒ 除日盘的像外地面纯黑（修之前这张是**一片全黑**）。
    {0.0f,  false, "r0.000_pure"},
    // ② 正常镜子观感：地面映天光与方块，日盘的像叠在上面。
    {0.0f,  true,  "r0.000_mirror"},
    // ③ θ 主导：r=0.02 的 a2 只有 θ 带宽的 3% ⇒ 应与 ① 几乎同一张图。
    {0.02f, false, "r0.020_pure"},
    // ④ 粗糙端由 roughness 主导：不能被这次改动弄没（本该有光）。
    {0.30f, true,  "r0.300_mirror"},
};
inline constexpr int kShotCount = 4;

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
}

}  // namespace jpov_specular_bandwidth

#endif  // JPOV_TEST_OBJECT3D_JPOV_SPECULAR_BANDWIDTH_GOLD_COMMON_H_

// JPOV Gold — 点光源的镜面带宽（逐像素角半径 θ = asin(R/dist)）
//
// 与平行光那份（jpov_specular_bandwidth_gold_common.h）同一个问题、同一张图的语言：
//   平行光的 θ 是常数（天体角半径），点光源的 θ **逐像素**、由灯泡的物理半径决定
//   （`PointLight::physical_radius`）。修之前那个字段只用来挪 `spec_dir`（Karis 代表点），
//   `a2` 一点没变 ⇒
//     · r=0 的镜面地面上**完全没有灯的高光**（δ × δ，D ≡ 0）；
//     · 有光泽的表面上也只有一个「点」高光，与灯多大无关（该糊的地方不糊）。
//   接上 `a2 += (θ/2)²` 后：灯越大，地上那块「灯的像」越大越柔（D 归一化 ⇒ 能量守恒，
//   只是峰值降、覆盖变宽）。
//
// 场景（人类可读）：与 skylight viewer 的镜面地板场景同一份资源与机位
//   （`InstallMirrorFloorScene()` + `SetPointSpecularView()`），地面 metallic=1 / roughness 可调；
//   太阳与环境光**全关** ⇒ 画面里唯一的照明就是那盏灯（否则太阳的高光会盖住要看的东西）。
//
// 机位与灯位**算死**（推导见 SkylightApp::PlacePointLampAtScreenCenter）：
//   灯放在「原点处的反射方向」上 3.5 m ⇒ 灯的像精确落在画面正中 ⇒ 测试可以按像素位置断言。
//   另画一个自发光小球（直径 = 2R）当灯的参照物，图上一眼能对上「天上的灯 / 地上的像」。
//
// 文件归属：generator（写仓库 gold PNG）与 test（渲染 + 定量断言）共用本头，
//   场景又来自 demo ⇒ 三方不可能分叉。

#ifndef JPOV_TEST_OBJECT3D_JPOV_POINT_SPECULAR_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_POINT_SPECULAR_GOLD_COMMON_H_

#include <string>

#include <glog/logging.h>

#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_point_specular {

inline constexpr int kImageWidth = 1280;
inline constexpr int kImageHeight = 720;

// 灯的像落在画面正中（见上面的机位推导），测试用它做位置断言。
inline constexpr int kExpectedCenterX = kImageWidth / 2;
inline constexpr int kExpectedCenterY = kImageHeight / 2;

// 一张「拍摄」= 一组对照条件。
struct Shot {
    float lamp_radius = 0.0f;      // PointLight::physical_radius（0 = 等效 δ）
    float floor_roughness = 0.0f;  // 地面 roughness（0 = 纯镜面）
    bool env_reflections = false;  // true = 开天光反射（镜子观感）
    const char* file_tag = "";
};

inline constexpr Shot kShots[] = {
    // ① 等效 δ（R=0）：修之前的行为 —— 地面上**没有**灯的像。
    {0.0f,  0.0f, false, "r0.000"},
    // ② 灯越来越大 ⇒ 地上的像越来越大（θ = asin(R/3.5 m)）。
    {0.02f, 0.0f, false, "r0.020"},
    {0.10f, 0.0f, false, "r0.100"},
    {0.50f, 0.0f, false, "r0.500"},
    // ③ 粗糙端由 roughness 主导：不能出现「修好光滑的、弄坏粗糙的」。
    {0.10f, 0.30f, false, "r0.100_rough0.300"},
    // ④ 普通镜子观感（开天光反射），灯的像叠在上面。
    {0.10f, 0.0f, true,  "r0.100_mirror"},
};
inline constexpr int kShotCount = 6;

inline std::string ShotFileName(const Shot& shot) {
    return std::string("point_specular_") + shot.file_tag + "_1280x720.png";
}

// 把 app 配成这一张拍摄的全部条件（generator 与 test 共用）。
// Pre-condition: app 已 Init()，且已调用 InstallMirrorFloorScene() / SetPointSpecularView()。
inline void ApplyShot(jpov_skylight::SkylightApp* app, const Shot& shot) {
    CHECK(app != nullptr);

    app->SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(shot.floor_roughness));
    app->SetPointLamp(shot.lamp_radius);

    if (shot.env_reflections) {
        app->SetEnvSpecular(true, 0.35f);
        app->SetSsr(true);
        app->ClearAmbientOverride();
    } else {
        // 画面里只留这盏灯：关 env/SSR + 环境光置零（太阳在 main 里已置零）。
        app->SetEnvSpecular(false);
        app->SetSsr(false);
        jpov::AmbientLight no_ambient{};
        no_ambient.intensity = 0.0f;
        app->SetAmbientOverride(no_ambient);
    }
}

// 场景装配（太阳置零 + 画面资源）：generator/test 共同的入口。
inline void InstallScene(jpov_skylight::SkylightApp* app) {
    CHECK(app != nullptr);
    app->InstallMirrorFloorScene();
    app->SetPointSpecularView();
    // 太阳置零：否则太阳的高光比这盏灯亮几个量级，会把要看的东西盖掉。
    // （天光 sky 仍留着：它是背景色，也是「开天光反射」那张图的反射源。）
    jpov::DirectionalLight no_sun{};
    no_sun.direction = {0.0f, -1.0f, 0.0f};
    no_sun.intensity = 0.0f;
    app->SetSunOverride(no_sun);
}

}  // namespace jpov_point_specular

#endif  // JPOV_TEST_OBJECT3D_JPOV_POINT_SPECULAR_GOLD_COMMON_H_

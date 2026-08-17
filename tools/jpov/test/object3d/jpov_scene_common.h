// JPOV 场景 gold 测试 —— 共享场景构建
//
// 同一套布景（石头墙 + 5×5 地面（左砖右土）+ 中央桌 + 桌边凳 + 桌上盆栽）
// + camera (3,3,3)，支持两种光照模式：
//   kPointLights（默认，旧 baseline）：3 点光源，无 sun/sky -> 与既有 point
//       light gold test 对比的历史基线。
//   kSun（新场景）：太阳平行光 + 阴影贴图 + 天光 ambient（与旧点光源版对比）。
//
// 本头文件被两个 generator/test 共用，保证两边场景几何完全一致，避免重复维护；
// 光照由 SceneApp::light_mode_ 切换。
//
// 注：PBR 光照在 llvmpipe 下三稳态非确定（见 jpov_pbr_cube_normal_gold_test
// 文件头注释，leader #16 决策），故本 test 不做逐像素颜色比对，只做 smoke
// check + 校验 gold image 存在供肉眼参考。

#ifndef JPOV_TEST_OBJECT3D_JPOV_SCENE_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_SCENE_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace jpov_scene {

// 场景光照模式。
enum class LightMode {
    kPointLights,  // 3 点光源（旧 baseline）
    kSun,          // 太阳平行光 + 阴影 + 天光 ambient（新场景）
};

// 需要在 scene_assets/ 下的子路径（build 时经 runfiles / GetProjectRoot 解析）
inline std::string AssetPath(const std::string& rel) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/test/object3d/scene_assets/" + rel;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/scene_assets/" + rel;
}

// 渲染应用：持有一组 GltfObject，OneIteration 里绘制整个布景。
class SceneApp : public JPOV {
public:
    using JPOV::JPOV;

    // 设置光照模式（默认 kPointLights = 旧 baseline）。
    void SetLightMode(LightMode mode) { light_mode_ = mode; }

    struct Slot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
    };
    std::vector<Slot> slots_;
    LightMode light_mode_ = LightMode::kPointLights;

    void AddSlot(jpov::GltfObject obj, jpov::Vec3f cen,
                 jpov::Vec3f up, jpov::Vec3f front) {
        slots_.push_back({std::move(obj), cen, up, front});
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // camera: 中心拉远，3/4 高角望向场景中心。坐标约定 z 正方向朝上
        //（历史 y-up → z-up，经绕 x 轴 +90° 旋转：y→z, z→-y）。
        const jpov::Vec3f scene_center = {0.0f, 0.0f, 0.6f};
        cmds->camera.position = {3.0f, 3.0f, 3.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.up       = {0.0f, 0.0f, 1.0f};   // z 正方向朝上
        cmds->camera.near     = 0.05f;

        if (light_mode_ == LightMode::kPointLights) {
            // 旧 baseline：3 点光源（z 朝上坐标，自旧 y-up (3,0,0)/(0,0,3)/(0,3,0)
            // 经绕 x+90° 变换：y→z, z→-y），白光 3,3,3,1, radius 6.0, physical 半径 0.5m。
            cmds->point_lights.push_back({
                {3.0f, 0.0f, 0.0f},  {3.0f, 3.0f, 3.0f, 1.0f}, 6.0f, 0.5f});
            cmds->point_lights.push_back({
                {0.0f, -3.0f, 0.0f}, {3.0f, 3.0f, 3.0f, 1.0f}, 6.0f, 0.5f});
            cmds->point_lights.push_back({
                {0.0f, 0.0f, 3.0f},  {3.0f, 3.0f, 3.0f, 1.0f}, 6.0f, 0.5f});
        } else if (light_mode_ == LightMode::kSun) {
            // 新场景：太阳平行光 + 阴影贴图 + 天光 ambient（z 朝上坐标）。
            // 太阳从斜上方（+z 侧）照射，产生清晰影子；天光做均匀补光。
            cmds->sun = jpov::DirectionalLight{
                /*direction*/ {0.4, 0.6, -0.7},     // 光方向：y 正,z 负（光射向 +y/-z）
                /*color*/ {1.0f, 0.98f, 0.92f},         // 暖白太阳
                /*intensity*/ 10.0f,
            };
            cmds->sky_light = jpov::SkyLight{
                /*sky_color*/ {0.55f, 0.82f, 1.00f, 1.0f},
                /*ground_color*/ {0.25f, 0.45f, 0.35f, 1.0f},
                /*intensity*/ 0.5f,   // 天光弱于太阳，突出阳光对比
            };
        }

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }
    }
};

// 加载并摆放整个布景（与 test 共用，保证场景一致）。
// app 需已 Init()。
inline void BuildScene(SceneApp* app) {
    // poly.pizza 家具 baseColor 极暗（木≈0.1/黑≈0.03），为可读性统一提亮。
    auto brighten = [](jpov::GltfObject* o, float k) {
        for (auto& p : o->primitives) {
            p.material.base_color.r *= k;
            p.material.base_color.g *= k;
            p.material.base_color.b *= k;
        }
    };
    // 加载单个模型并 CHECK 非空。
    auto load = [app](const std::string& rel) {
        jpov::GltfObject o = app->LoadGltf(AssetPath(rel));
        CHECK(!o.empty()) << "LoadGltf failed: " << rel;
        return o;
    };

    // 地面：5×5 = 25 块 6x6 小 quad 平铺成整片地板，砖块尺寸正确。
    // 左半(x<0)砖地、右半(x≥0)土地。z 朝上：地板在 xy 平面，整体下移 0.5（z=-0.5）。
    for (int iy = 0; iy < 5; ++iy) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f;
            const float gy = -12.0f + iy * 6.0f;
            const bool brick = (gx < 0.0f);
            app->AddSlot(load(brick ? "ground/ground_brick.gltf"
                                    : "ground/ground_dirt.gltf"),
                         {gx, gy, -0.5f}, {0,0,1}, {0,1,0});
        }
    }

    // 墙: 石头墙板。baseColor+normal 来自 glTF，MR 手动填（同 cube test），
    // 不带 AO / emissive。z 朝上：up=(0,0,1)，front=(0,1,0) 立起。
    {
        jpov::GltfObject wall = load("wall_rock/wall_rock.gltf");
        const uint32_t met = app->RegisterTexture(AssetPath("wall_rock/rock_metallic.png"));
        const uint32_t rgh = app->RegisterTexture(AssetPath("wall_rock/rock_roughness.png"));
        for (auto& p : wall.primitives) {
            p.material.has_metallic_tex = true;
            p.material.metallic_tex = met;
            p.material.has_roughness_tex = true;
            p.material.roughness_tex = rgh;
            p.material.ao_tex = 0;
            p.material.emissive_tex = 0;
        }
        app->AddSlot(std::move(wall), {0.0f, -2.75f, -0.5f}, {0,-1,0}, {0,0,1});
    }

    // 桌子: 中央。z 朝上：up=(-1,0,0)（桌面法线指向场景外 -x）
    jpov::GltfObject table = load("table.glb");
    brighten(&table, 3.0f);
    app->AddSlot(std::move(table), {0.0f, -0.2f, 0.71f}, {-1,0,0}, {0,0,1});

    // 凳子: 桌旁。z 朝上：up=(0,-1,0)，front=(0,0,1)
    jpov::GltfObject stool = load("stool.glb");
    brighten(&stool, 3.0f);
    app->AddSlot(std::move(stool), {0.7f, 0.9f, 0.27f}, {0,-1,0}, {0,0,1});

    // 盆栽: 桌上。z 朝上：up=(1,0,0)
    jpov::GltfObject plant = load("houseplant.glb");
    brighten(&plant, 4.0f);
    app->AddSlot(std::move(plant), {0.3f, -0.3f, 1.84f}, {1,0,0}, {0,0,1});
}

}  // namespace jpov_scene

#endif  // JPOV_TEST_OBJECT3D_JPOV_SCENE_COMMON_H_

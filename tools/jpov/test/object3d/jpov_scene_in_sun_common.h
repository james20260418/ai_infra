// JPOV 场景 gold 测试 —— 共享场景构建（阳光版）
//
// 从 jpov_scene_common.h 复制：石头墙 + 5×5 地面（左砖右土）+ 中央桌 +
// 桌边凳 + 桌上盆栽，场景几何/材质完全一致。
//
// 与点光源版唯一区别在光照（"只改光照"）：去掉 3 个点光源，改用太阳平行光
//  `cmds->sun`，方向 (0,-1,-1)（从斜上方照向场景），走 DirectionalLight +
// 级联阴影（CSM, ShadowConfig::Default()）。用于整体查看太阳光+阴影效果。
//
// 本头文件被 generator（写仓库 gold image）和 test（渲染+smoke check）共用，
// 保证两边场景完全一致，避免重复维护。
//
// 注：PBR 光照在 llvmpipe 下三稳态非确定（见 jpov_pbr_cube_normal_gold_test
// 文件头注释，leader #16 决策），故本 test 不做逐像素颜色比对，只做 smoke
// check + 校验 gold image 存在供肉眼参考。

#ifndef JPOV_TEST_OBJECT3D_JPOV_SCENE_IN_SUN_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_SCENE_IN_SUN_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace jpov_scene_in_sun {

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
class SceneSunApp : public JPOV {
public:
    using JPOV::JPOV;

    struct Slot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
    };
    std::vector<Slot> slots_;

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

        // camera: 中心拉远到 (3,3,3)，3/4 高角望向场景中心（同点光源版）
        const jpov::Vec3f scene_center = {0.0f, 0.6f, 0.0f};
        cmds->camera.position = {3.0f, 3.0f, 3.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.near     = 0.05f;

        // ── 光照：只改光照 ──
        // 去掉点光源版 3 点光源，改用太阳平行光（DirectionalLight）。
        // direction=(0,-1,-1)：光从斜上方（+y 略偏 +z）照向场景，产生清晰影子。
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {0.0f, -1.0f, -1.0f},
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }
    }
};

// 加载并摆放整个布景（与 test 共用，保证场景一致）。
// app 需已 Init()。场景几何/材质与点光源版完全一致。
inline void BuildScene(SceneSunApp* app) {
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
    // 左半(x<0)砖地、右半(x≥0)土地。地面整体下移 0.5（配合点光源高度）。
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f;
            const float gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            app->AddSlot(load(brick ? "ground/ground_brick.gltf"
                                    : "ground/ground_dirt.gltf"),
                         {gx, -0.5f, gz}, {0,1,0}, {0,0,1});
        }
    }

    // 墙: 石头墙板。baseColor+normal 来自 glTF，MR 手动填（同 cube test），
    // 不带 AO / emissive。
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
        app->AddSlot(std::move(wall), {0.0f, 0.0f, -2.75f}, {0,0,1}, {0,1,0});
    }

    // 桌子: 中央, 图上上方为 +Y → up=(-1,0,0), front=(0,1,0)
    jpov::GltfObject table = load("table.glb");
    brighten(&table, 3.0f);
    app->AddSlot(std::move(table), {0.0f, 0.71f, 0.2f}, {-1,0,0}, {0,1,0});

    // 凳子: 桌旁, 方向矢量绕 X 轴 -90°: up(0,1,0)→(0,0,1), front(0,0,1)→(0,1,0)
    jpov::GltfObject stool = load("stool.glb");
    brighten(&stool, 3.0f);
    app->AddSlot(std::move(stool), {0.7f, 0.27f, 0.9f}, {0,0,1}, {0,1,0});

    // 盆栽: 桌上, front=(0,1,0) 把 0.38 高立起
    jpov::GltfObject plant = load("houseplant.glb");
    brighten(&plant, 4.0f);
    app->AddSlot(std::move(plant), {0.3f, 1.84f, 0.3f}, {1,0,0}, {0,1,0});
}

}  // namespace jpov_scene_in_sun

#endif  // JPOV_TEST_OBJECT3D_JPOV_SCENE_IN_SUN_COMMON_H_

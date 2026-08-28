// JPOV picking + highlight —— standard_sunny_day 基底独立测试 common
//
// 以 standard_sunny_day（石头墙 + 5×5 地面 + 桌/凳/盆栽 + Preetham 天光 +
// 太阳平行光 + CSM 阴影）为底座，验证 GPU color-ID 拾取 + 方法 B 高亮边框。
//
// 与 standard_sunny_day 的差异：
//   - 相机视角完全复用（position={4,2.5,4} → target={0,2.5,0}，near=0.05）
//   - 场景构建复用 BuildScene（摆位一致：墙/桌/凳/盆栽）
//   - 新增拾取/高亮能力：每个 slot 可带 picking_id + highlight，OneIteration 透传
//   - 高亮目标：stool（凳子，前景中央）+ wall（墙，右侧）
//
// 独立 common（不修改 jpov_standard_sunny_day_common.h），供本 test 的
// generator/test 共用。

#ifndef JPOV_TEST_OBJECT3D_SKYDOME_PICK_HIGHLIGHT_SUNNY_COMMON_H_
#define JPOV_TEST_OBJECT3D_SKYDOME_PICK_HIGHLIGHT_SUNNY_COMMON_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace jpov_pick_highlight_sunny {

// scene_assets/ 下子路径（build 时经 runfiles / GetProjectRoot 解析）。
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

// 高亮对象 picking_id（供 test 断言 + generator/test 引用，需 >0 才可拾取）。
inline constexpr uint32_t kIdStool = 1;  // 凳子
inline constexpr uint32_t kIdWall  = 2;  // 墙

// 拾取断言用的窗口像素坐标（640x360 窗口系，由 jpov_focus_probe 校准）。
inline constexpr int kProbeStoolX = 310;  // 凳子中心
inline constexpr int kProbeStoolY = 304;
inline constexpr int kProbeWallX  = 410;  // 墙（右侧大板，任意命中点）
inline constexpr int kProbeWallY  = 200;

// 渲染应用：持有一组 GltfObject（含拾取/高亮标注），OneIteration 画布景 + 天光。
class PickHighlightSunnyApp : public JPOV {
public:
    using JPOV::JPOV;

    // 拾取查询配置（OneIteration 前设置，默认关零开销）。
    bool   pick_enabled = false;
    float  pick_x       = 0.0f;
    float  pick_y       = 0.0f;

    // 是否绘制高亮边框（全局开关，配合 highlight_style）。默认画。
    bool   draw_highlight = true;

    struct Slot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
        uint32_t picking_id = 0;  // 0=不可拾取
        bool highlight      = false;
    };
    std::vector<Slot> slots_;

    void AddSlot(jpov::GltfObject obj, jpov::Vec3f cen,
                 jpov::Vec3f up, jpov::Vec3f front,
                 uint32_t picking_id = 0, bool highlight = false) {
        slots_.push_back({std::move(obj), cen, up, front, picking_id, highlight});
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

        // ── 相机：复用 standard_sunny_day 视角 ──
        const jpov::Vec3f scene_center = {0.0f, 2.5f, 0.0f};
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.near     = 0.05f;

        // ── 太阳平行光 ──
        const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ sun_light_dir,
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        // ── 环境光（正午阴影基准）──
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f},
            .intensity = 0.3f,
        };

        // ── 天光（Preetham）—— 与 standard_sunny_day 一致 ──
        cmds->sky = jpov::DaySkyCommand{
            /*sun_dir*/ jpov::Vec3f(-sun_light_dir.x(), -sun_light_dir.y(),
                                    -sun_light_dir.z()),
            /*turbidity*/ 2.0f,
            /*season*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 1.0f,
            /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
            /*sun_radius*/ 0.02,
            /*sun_brightness*/ 1e3,
            /*sun_glow*/ 1.0,
        };

        cmds->tone_mapping = true;

        // 全局高亮样式（统一金黄边框，屏幕恒定像素宽）。draw_highlight=false 时不设，零开销。
        if (draw_highlight) {
            cmds->highlight_style = jpov::HighlightStyle{
                .color = {1.0f, 0.85f, 0.3f, 1.0f},
                .outline_px = 2,
            };
        }

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front,
                                 s.picking_id, s.highlight);
        }

        // 拾取查询（enabled 默认 false，零开销）。
        cmds->pick.enabled  = pick_enabled;
        cmds->pick.screen_x = pick_x;
        cmds->pick.screen_y = pick_y;
    }
};

// 加载并摆放整个布景（摆位与 standard_sunny_day 的 BuildScene 一致）。
// highlight_stool / highlight_wall：是否把对应对象标成高亮（且可拾取）。
inline void BuildScene(PickHighlightSunnyApp* app,
                       bool highlight_stool, bool highlight_wall) {
    auto brighten = [](jpov::GltfObject* o, float k) {
        for (auto& p : o->primitives) {
            p.material.base_color.r *= k;
            p.material.base_color.g *= k;
            p.material.base_color.b *= k;
        }
    };
    auto load = [app](const std::string& rel) {
        jpov::GltfObject o = app->LoadGltf(AssetPath(rel));
        CHECK(!o.empty()) << "LoadGltf failed: " << rel;
        return o;
    };

    // 地面：5×5 = 25 块 6x6 小 quad 平铺。
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f;
            const float gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            app->AddSlot(load(brick ? "ground/ground_brick.gltf"
                                    : "ground/ground_dirt.gltf"),
                         {gx, -0.5f, gz}, {0, 1, 0}, {0, 0, 1});
        }
    }

    // 墙：石头墙板（可高亮/可拾取）。
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
        app->AddSlot(std::move(wall), {0.0f, 0.0f, -2.75f}, {0, 0, 1}, {0, 1, 0},
                     /*picking_id=*/kIdWall, /*highlight=*/highlight_wall);
    }

    // 桌子 / 凳子（可高亮/可拾取）/ 盆栽
    jpov::GltfObject table = load("table.glb");
    brighten(&table, 3.0f);
    app->AddSlot(std::move(table), {0.0f, 0.71f, 0.2f}, {-1, 0, 0}, {0, 1, 0});

    jpov::GltfObject stool = load("stool.glb");
    brighten(&stool, 3.0f);
    app->AddSlot(std::move(stool), {0.7f, 0.27f, 0.9f}, {0, 0, 1}, {0, 1, 0},
                 /*picking_id=*/kIdStool, /*highlight=*/highlight_stool);

    jpov::GltfObject plant = load("houseplant.glb");
    brighten(&plant, 4.0f);
    app->AddSlot(std::move(plant), {0.3f, 1.84f, 0.3f}, {1, 0, 0}, {0, 1, 0});
}

}  // namespace jpov_pick_highlight_sunny

#endif  // JPOV_TEST_OBJECT3D_SKYDOME_PICK_HIGHLIGHT_SUNNY_COMMON_H_

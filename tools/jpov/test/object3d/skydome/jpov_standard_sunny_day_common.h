// JPOV 标准晴天（standard_sunny_day）gold 测试 —— 共享场景构建
//
// 以 scene_in_sun（石头墙 + 5×5 地面 + 桌/凳/盆栽）为模板，叠加**程序化天光**
// （DaySkyCommand + DirectionalLight 对齐）：
//   - 太阳平行光 DirectionalLight：direction=(0,-1,-1)（光从斜上方 +y 略偏 +z 照向场景）
//   - 天光 DaySkyCommand：sun_dir 与 DirectionalLight 对齐（sun_dir = -direction），
//     turbidity=2（清澈晴天）、season 中性、intensity=1.0（用户调参目标：正午晴天）
//   - tone_mapping = true（HDR 链路，ACES）
//   - ambient = 0.3（正午阴影基准，见 LIGHT_INTENSITY.md 晴天基准值）
//
// 用于调试：让天空背景（Preetham 天光）+ 太阳直射 + 阴影在 HDR 链路上
// 正确对齐、协调一致。SKY_LUMINANCE_SCALE 保留 0.04，天色的绝对亮度由
// sky.intensity 调节（Danis 手动调参）。
//
// 本头文件被 generator（写仓库 gold image）和 test（渲染+smoke check）共用。

#ifndef JPOV_TEST_OBJECT3D_SKYDOME_STANDARD_SUNNY_DAY_COMMON_H_
#define JPOV_TEST_OBJECT3D_SKYDOME_STANDARD_SUNNY_DAY_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace jpov_standard_sunny_day {

// scene_assets/ 下的子路径（build 时经 runfiles / GetProjectRoot 解析）
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

// 渲染应用：持有一组 GltfObject，OneIteration 画布景 + 天光。
class StandardSunnyDayApp : public JPOV {
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

        const jpov::Vec3f scene_center = {0.0f, 2.5f, 0.0f};
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.near     = 0.05f;

        // ── 太阳平行光 ──
        // 光传播方向：从天空斜上方照向场景（+y 偏 +z）。太阳在天球上与此相对。
        const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ sun_light_dir,
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        // ── 环境光（正午阴影基准，见 LIGHT_INTENSITY.md 三·五）──
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f},
            .intensity = 0.3f,
        };

        // ── 天光背景（Preetham）──
        // sun_dir 与 DirectionalLight 对齐：太阳在天球上的方向 = 光传播的反方向。
        // 正午晴天基准：turbidity=2（清澈）、season 中性、intensity=1.0。
        // 太阳盘：sun_radius=0.02（调试放大以便看清盘）、sun_brightness=1e3
        // （降基数以显示盘边缘渐变，纯物理 2e5 会 ACES 饱和成纯白小点）、sun_glow=1.0。
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

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }
    }
};

// 加载并摆放整个布景（与 scene_in_sun 一致）。
inline void BuildScene(StandardSunnyDayApp* app) {
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

    // 墙：石头墙板。
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
        app->AddSlot(std::move(wall), {0.0f, 0.0f, -2.75f}, {0, 0, 1}, {0, 1, 0});
    }

    // 桌子 / 凳子 / 盆栽
    jpov::GltfObject table = load("table.glb");
    brighten(&table, 3.0f);
    app->AddSlot(std::move(table), {0.0f, 0.71f, 0.2f}, {-1, 0, 0}, {0, 1, 0});

    jpov::GltfObject stool = load("stool.glb");
    brighten(&stool, 3.0f);
    app->AddSlot(std::move(stool), {0.7f, 0.27f, 0.9f}, {0, 0, 1}, {0, 1, 0});

    jpov::GltfObject plant = load("houseplant.glb");
    brighten(&plant, 4.0f);
    app->AddSlot(std::move(plant), {0.3f, 1.84f, 0.3f}, {1, 0, 0}, {0, 1, 0});
}

}  // namespace jpov_standard_sunny_day

#endif  // JPOV_TEST_OBJECT3D_SKYDOME_STANDARD_SUNNY_DAY_COMMON_H_

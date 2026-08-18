// JPOV Skydome gold 测试 —— 共享场景构建（参数化：时刻 × 机位）
//
// 验证程序化天光（Preetham）在不同时刻（白天/晚霞/夜间）× 双机位（从日月
// 看场景中心 / 从场景中心看日月）下的表现。同一布景（石头墙 + 5×5 地面 +
// 桌/凳/盆栽），只改天光参数与相机姿态。
//
// 生成 6 张 gold（时刻 × 机位）：
//   白天   day    : sun_elev≈45, turb≈2（清澈晴天，太阳圆盘）
//   晚霞   sunset : sun_elev≈8,  turb≈4（低太阳，红橙霞 + 长影）
//   夜间   night  : sun_elev=-5, is_moon（月亮圆盘，冷蓝夜空）
//
// 机位：
//   kCamFromBody: 从日月位置（沿太阳/月亮方向远处）望向场景中心 —— 看场景被
//                 天光侧照的效果 + 地平线四周天空。
//   kCamToBody:   从场景中心望向日月（当前默认机位）—— 看太阳/月亮圆盘 +
//                 天空方向不对称（朝日查看红橙，背面偏暗）。
//
// 本头文件被 generator（写 6 张 gold）和 test（smoke check）共用。

#ifndef JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SKYDOME_COMMON_H_
#define JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SKYDOME_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace jpov_skydome {

// 时刻预设
enum class TimePreset {
    kDay,     // 白天
    kSunset,  // 晚霞
    kNight,   // 夜间
};
inline const char* TimePresetName(TimePreset t) {
    switch (t) {
        case TimePreset::kDay:    return "day";
        case TimePreset::kSunset: return "sunset";
        case TimePreset::kNight:  return "night";
    }
    return "?";
}

// 机位预设
enum class CamPreset {
    kFromBody,  // 从日月看向场景中心
    kToBody,    // 从场景中心看向日月（默认）
};

// 场景资产路径（复用 object3d/scene_assets，经 GetProjectRoot / runfiles 解析）
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

// 渲染应用：持有一组 GltfObject；OneIteration 按 时刻×机位 设置相机与天光。
class SkydomeSceneApp : public JPOV {
public:
    using JPOV::JPOV;

    TimePreset time_ = TimePreset::kDay;
    CamPreset  cam_  = CamPreset::kToBody;

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

    // 按时刻得到天光参数
    static jpov::SkyCommand SkyFor(TimePreset t) {
        switch (t) {
            case TimePreset::kDay:
                return {
                    /*elev*/ 45.0f, /*azim*/ 0.0f, /*turb*/ 2.0f,
                    /*season*/ {1,1,1,1}, /*intensity*/ 1.0f,
                    /*ground*/ {0.08f,0.09f,0.11f,1.0f},
                    /*is_moon*/ false, /*body*/ {1.0f,1.0f,0.95f,1.0f},
                    /*radius*/ 0.04f, /*glow*/ 0.05f,
                };
            case TimePreset::kSunset:
                return {
                    /*elev*/ 8.0f, /*azim*/ 0.0f, /*turb*/ 4.0f,
                    /*season*/ {1.0f,0.92f,0.85f,1.0f}, /*intensity*/ 1.0f,
                    /*ground*/ {0.05f,0.05f,0.07f,1.0f},
                    /*is_moon*/ false, /*body*/ {1.0f,0.7f,0.35f,1.0f},
                    /*radius*/ 0.045f, /*glow*/ 0.1f,
                };
            case TimePreset::kNight:
                return {
                    /*elev*/ -5.0f, /*azim*/ 0.0f, /*turb*/ 2.0f,
                    /*season*/ {0.7f,0.8f,1.0f,1.0f}, /*intensity*/ 1.0f,
                    /*ground*/ {0.02f,0.02f,0.04f,1.0f},
                    /*is_moon*/ true,  /*body*/ {0.9f,0.92f,1.0f,1.0f},
                    /*radius*/ 0.035f, /*glow*/ 0.05f,
                };
        }
        return {};
    }

    // 太阳位置（方向向量，用于相机"从日月看"机位）
    static jpov::Vec3f SunDirFor(TimePreset t) {
        return SkyFor(t).SunDir();
    }

    // 场景中心（地面场景的视觉重心）
    static const jpov::Vec3f SceneCenter() { return {0.0f, 0.8f, 0.0f}; }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        cmds->camera.near = 0.05f;

        jpov::SkyCommand sky = SkyFor(time_);
        const jpov::Vec3f sun_dir = sky.SunDir();
        const jpov::Vec3f center = SceneCenter();

        // ── 机位 ──
        if (cam_ == CamPreset::kToBody) {
            // 从场景中心望向日月：从高处看向太阳/月亮方向，避免被地面几何
            // 遮挡天体圆盘（视线向上、远处物体都在下方），天体圆盘居中可见。
            cmds->camera.position = {0.0f, 5.0f, -6.0f};
            cmds->camera.target   = cmds->camera.position + sun_dir * 15.0f;
        } else {
            // 从日月望向场景：相机在太阳/月亮方向的中等高度处，看向远处地平线
            //（近水平），让画面同时呈现：天空圆顶（上部）+ 场景（下部地平线前）。
            cmds->camera.position = center + sun_dir * 7.0f;
            cmds->camera.target   = {0.0f, 0.4f, -14.0f};
        }
        cmds->camera.up = {0.0f, 1.0f, 0.0f};

        // ── 光照：太阳平行光（与天光太阳位置对齐，方向=-SunDir）──
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-sun_dir.x(), -sun_dir.y(), -sun_dir.z()},
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        // ── 天光 ──
        cmds->sky = sky;

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }
    }
};

// 加载并摆放整个布景（与 test 共用，保证场景一致）。
inline void BuildScene(SkydomeSceneApp* app) {
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
                         {gx, -0.5f, gz}, {0,1,0}, {0,0,1});
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
        app->AddSlot(std::move(wall), {0.0f, 0.0f, -2.75f}, {0,0,1}, {0,1,0});
    }

    // 桌子 / 凳子 / 盆栽
    jpov::GltfObject table = load("table.glb");
    brighten(&table, 3.0f);
    app->AddSlot(std::move(table), {0.0f, 0.71f, 0.2f}, {-1,0,0}, {0,1,0});

    jpov::GltfObject stool = load("stool.glb");
    brighten(&stool, 3.0f);
    app->AddSlot(std::move(stool), {0.7f, 0.27f, 0.9f}, {0,0,1}, {0,1,0});

    jpov::GltfObject plant = load("houseplant.glb");
    brighten(&plant, 4.0f);
    app->AddSlot(std::move(plant), {0.3f, 1.84f, 0.3f}, {1,0,0}, {0,1,0});
}

}  // namespace jpov_skydome

#endif  // JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SKYDOME_COMMON_H_

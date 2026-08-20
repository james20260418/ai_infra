// JPOV 太阳轨迹（sun path）gold 测试 —— 共享场景构建
//
// 生成太阳从早到晚的全部天光时段图片。核心抽象：
//   SkyDirectionAmbient —— 一个完整的天光时段配置，打包三类光照：
//     1. sky（天光背景 DaySkyCommand，Preetham 程序化天空）
//     2. sun（太阳方向光 DirectionalLight，可带阴影）
//     3. ambient（环境光 AmbientLight，背阳面补光）
//   vector<SkyDirectionAmbient> 即一条太阳轨迹配置序列，遍历每个配置渲染一张图，
//   图片后缀用 index（sun_path_<index>.png）。先放一个 standard（正午晴天）配置
//   （vector 尺寸 = 1），后续手动往 vector 里加配置来调不同太阳角度。
//
// 相机与场景复用 jpov_standard_sunny_day（石头墙 + 5×5 地面 + 桌/凳/盆栽，
// 相机 position {4,2.5,4} target {0,2.5,0} near 0.05），只改光照参数。
//
// 本头文件被 generator（写 N 张 gold）和 test（smoke check）共用。

#ifndef JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_COMMON_H_
#define JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/test/object3d/skydome/jpov_standard_sunny_day_common.h"
#include "tools/common/utils.h"

namespace jpov_sun_path {

// 一个完整的天光时段配置：天光 + 太阳方向光 + 环境光。
struct SkyDirectionAmbient {
    jpov::DaySkyCommand  sky;      // 天光背景（Preetham 程序化天空）
    jpov::DirectionalLight sun;    // 太阳方向光（带阴影）
    jpov::AmbientLight   ambient;  // 环境光（背阳面补光）
};

// 太阳轨迹渲染 app：复用 standard_sunny_day 的场景与相机，
// 光照参数由 configs_ 序列按 active_index_ 驱动。
class SunPathApp : public jpov_standard_sunny_day::StandardSunnyDayApp {
public:
    using jpov_standard_sunny_day::StandardSunnyDayApp::StandardSunnyDayApp;

    std::vector<SkyDirectionAmbient> configs_;
    int active_index_ = 0;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ── 相机（同 standard_sunny_day）──
        const jpov::Vec3f scene_center = {0.0f, 2.5f, 0.0f};
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.near     = 0.05f;

        // ── 光照：从 config 序列取当前时段的配置 ──
        CHECK(active_index_ >= 0 && active_index_ < static_cast<int>(configs_.size()))
            << "active_index_ 越界: " << active_index_;
        const SkyDirectionAmbient& cfg = configs_[active_index_];

        cmds->sky = cfg.sky;
        cmds->sun = cfg.sun;
        cmds->ambient = cfg.ambient;
        cmds->tone_mapping = true;

        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }
    }
};

// 默认配置序列：先放一个 standard（正午晴天）配置，vector 尺寸 = 1。
// 后续往这里加 SkyDirectionAmbient 即可扩展太阳轨迹时段。
inline std::vector<SkyDirectionAmbient> DefaultSunPath() {
    std::vector<SkyDirectionAmbient> path;

    SkyDirectionAmbient noon;  // 正午晴天（与 standard_sunny_day 完全一致）

    // 太阳方向光：光从斜上方 +y 偏 +z 照向场景（光传播方向）。
    const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
    noon.sun = jpov::DirectionalLight{
        /*direction*/ sun_light_dir,
        /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
        /*intensity*/ 3.0f,
    };

    // 环境光（正午阴影基准，见 LIGHT_INTENSITY.md 三·五）。
    noon.ambient = jpov::AmbientLight{
        .color = {1.0f, 1.0f, 1.0f, 1.0f},
        .intensity = 0.3f,
    };

    // 天光背景（Preetham）：sun_dir 与方向光对齐（太阳在天球上 = 光传播反方向）。
    noon.sky = jpov::DaySkyCommand{
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

    path.push_back(std::move(noon));
    return path;
}

}  // namespace jpov_sun_path

#endif  // JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_COMMON_H_

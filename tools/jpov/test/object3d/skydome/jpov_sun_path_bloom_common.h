// JPOV 太阳轨迹 + 辉光（bloom）gold 测试 —— 共享场景构建
//
// 复用 sun_path（太阳轨迹：正午/黄昏多时段天光场景），叠加 Bloom 后处理：
//   - 场景/相机/光照完全同 sun_path（石头墙 + 5×5 地面 + 桌/凳/盆栽，
//     相机 {4,2.5,4}→{0,2.5,0}，sun_path_1 = 低仰角黄昏）。
//   - 区别：本 app 额外暴露 bloom_enabled / bloom_cfg，OneIteration 里按
//     开关给 cmds->bloom 赋 BloomConfig。
//   - 黄昏（sun_path_1）太阳盘 brightness=1e3、天空很暗 —— 是理想的高亮
//     辉光基底：太阳盘周围应出现一圈可辨识的柔和光晕，而暗部（无 >threshold
//     的亮度）不受影响。
//
// 本头文件被 generator（写 bloom 开/关两张 gold）和 test（smoke + 开/关差异
// 门禁）共用，保证渲染链路一致。

#ifndef JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_BLOOM_COMMON_H_
#define JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_BLOOM_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/test/object3d/skydome/jpov_sun_path_common.h"
#include "tools/common/utils.h"

namespace jpov_sun_path_bloom {

// 基于 sun_path 的 bloom 渲染 app：在 SunPathApp 之上叠加可开关的 Bloom。
class SunPathBloomApp : public jpov_sun_path::SunPathApp {
public:
    using jpov_sun_path::SunPathApp::SunPathApp;

    // 开关：true 时给 cmds->bloom 赋 bloom_cfg；false 时不留 bloom（零开销）。
    bool bloom_enabled = false;
    jpov::BloomConfig bloom_cfg;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        // 先复用 sun_path 的场景/光照/相机构建。
        jpov_sun_path::SunPathApp::OneIteration(frame_count, input, winfo, cmds);

        // 叠加 bloom。
        if (bloom_enabled) {
            bloom_cfg.enabled = true;
            cmds->bloom = bloom_cfg;
        } else {
            cmds->bloom.reset();
        }
    }
};

// 黄昏（sun_path index=1）的 bloom 默认参数：太阳盘 brightness=1e3、天空暗，
// threshold 取 1.0（>1 才算高亮），intensity 适中，levels 给 3 级让光晕看得清。
inline jpov::BloomConfig DefaultBloom() {
    jpov::BloomConfig cfg;
    cfg.intensity = 0.8f;
    cfg.threshold = 1.0f;
    cfg.levels = 3;
    return cfg;
}

}  // namespace jpov_sun_path_bloom

#endif  // JPOV_TEST_OBJECT3D_SKYDOME_JPOV_SUN_PATH_BLOOM_COMMON_H_

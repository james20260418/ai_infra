// JPOV postprocess — 最终亮度/对比度微调 gold test 共用场景构建。
//
// 基底场景复用 object3d/pliers（glTF Pliers PBR 全通道）：
//   - 相机 3/4 视角完整看 pliers（长轴沿 Z）
//   - 三光源对称 + ambient
//   - 通过 final_contrast / final_brightness 控制后处理的最终微调
//
// generator 与 test 共用此文件，避免场景参数分叉（教训：mrquad gen/test
// 分叉导致 gold diff 飙升）。改动场景参数必须两边一致。

#ifndef JPOV_TEST_POSTPROCESS_FINAL_ADJUST_COMMON_H_
#define JPOV_TEST_POSTPROCESS_FINAL_ADJUST_COMMON_H_

#include <cstdlib>
#include <string>
#include <utility>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"

namespace jpov {
namespace test {

// 最终微调参数（对比度/亮度），由 generator/test 各自设置后渲染。
struct FinalAdjustParams {
    float contrast   = 1.0f;
    float brightness = 0.0f;
};

// 以 pliers 为基底的最终微调渲染 app。
// 通过 SetFinalAdjust 设置 final_contrast / final_brightness。
class FinalAdjustApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetFinalAdjust(const FinalAdjustParams& p) {
        contrast_   = p.contrast;
        brightness_ = p.brightness;
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机与 pliers gold test 一致：3/4 视角完整看 pliers（长轴沿 Z）。
        // near=0.01 避免近裁剪面切掉朝相机一侧的手柄/钳口（模型极小）。
        const float cx = 0.0075f;
        const float cz = -0.04f;
        cmds->camera.position = {0.075f, 0.08f, cz + 0.045f};
        cmds->camera.target   = {cx, 0.0f, cz};
        cmds->camera.near     = 0.01f;

        // 三光源对称照明（同 pliers gold test）
        cmds->point_lights.push_back({
            {0.15f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.5f, 0.01f, 1.0f});
        cmds->point_lights.push_back({
            {0.0f, 0.15f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.5f, 0.01f, 1.0f});
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 0.15f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.5f, 0.01f, 1.0f});

        // Ambient：适中
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f},
            .intensity = 0.5f
        };

        // 最终微调参数（被测接口）
        cmds->tone_mapping = true;
        cmds->srgb_encode = true;
        cmds->final_contrast   = contrast_;
        cmds->final_brightness = brightness_;

        // 绘制整个 glTF 对象（pliers，内部展开为多个 DrawObject3D）
        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

    // 设置 gltf 对象（App 生命周期内持有）。
    void SetGltfObject(jpov::GltfObject obj) { gltf_ = std::move(obj); }

    // glTF 加载路径评估（TEST_SRCDIR 或 project root）。
    static std::string GetGltfPath() {
        const char* test_srcdir = std::getenv("TEST_SRCDIR");
        if (test_srcdir) {
            std::string p = test_srcdir;
            if (!p.empty() && p.back() != '/') p.push_back('/');
            p += "__main__/tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
            return p;
        }
        return jpov::GetProjectRoot() +
            "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
    }

private:
    jpov::GltfObject gltf_;
    float contrast_ = 1.0f;
    float brightness_ = 0.0f;
};

// 渲染单帧到 outpath（设置指定最终微调参数）。App 需已 Init + 已 SetGltfObject。
// 供 generator / test 共用，避免渲染逻辑分叉。多次 RunOnce 复用同一 app。
inline void RenderFinalAdjust(FinalAdjustApp* app,
                              const std::string& outpath,
                              const FinalAdjustParams& params) {
    app->SetFinalAdjust(params);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app->RunOnce(input, winfo, outpath.c_str());
}

}  // namespace test
}  // namespace jpov

#endif  // JPOV_TEST_POSTPROCESS_FINAL_ADJUST_COMMON_H_

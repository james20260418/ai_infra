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

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"

namespace jpov {
namespace test {

// 最终微调参数（对比度/亮度/曝光），由 generator/test 各自设置后渲染。
// exposure 作用于 tone map 前的 HDR 值（fixed EV）；contrast/brightness
// 作用于 sRGB 编码后的最终值。
struct FinalAdjustParams {
    float contrast   = 1.0f;
    float brightness = 0.0f;
    float exposure   = 1.0f;
};

// 以 pliers 为基底的最终微调渲染 app。
// 通过 SetFinalAdjust 设置 final_contrast / final_brightness。
class FinalAdjustApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetFinalAdjust(const FinalAdjustParams& p) {
        contrast_   = p.contrast;
        brightness_ = p.brightness;
        exposure_   = p.exposure;
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
        cmds->exposure         = exposure_;

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
    float exposure_ = 1.0f;
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

// ---- 后处理 test 统计工具：读 PNG 亮度 + 物体 mask 统计（供各 gold test 复用）----

// 读取 PNG 全部像素灰度亮度（[0,255]，含背景）。
inline bool LoadLumPng(const std::string& path, std::vector<float>* lum,
                       int* width, int* height) {
    int w = 0, h = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!px) {
        LOG(ERROR) << "Failed to load " << path;
        return false;
    }
    lum->clear();
    lum->reserve(w * h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* q = &px[i * 4];
        lum->push_back((q[0] + q[1] + q[2]) / 3.0f);
    }
    stbi_image_free(px);
    *width = w;
    *height = h;
    return true;
}

// 物体的亮色统计（物体像素 = 非黑背景；几何不随后处理参数变，故 mask 跨参数有效）。
struct LumStats {
    double min_v;
    double max_v;
    double mean;
    double std;
};

// 对非黑像素（物体区域）统计 min/max/mean/std。mask 可来自另一张图（共享几何）。
// Pre-condition: lum.size() == mask.size()
inline LumStats ComputeMaskedStats(const std::vector<float>& lum,
                                   const std::vector<bool>& mask) {
    CHECK_EQ(lum.size(), mask.size())
        << "ComputeMaskedStats: lum/mask 尺寸不一致";
    double sum = 0.0, sq = 0.0, mins = 256.0, maxs = -1.0;
    long n = 0;
    for (size_t i = 0; i < lum.size(); ++i) {
        if (!mask[i]) continue;
        const double v = lum[i];
        sum += v;
        sq += v * v;
        if (v < mins) mins = v;
        if (v > maxs) maxs = v;
        ++n;
    }
    LumStats s;
    s.min_v = mins;
    s.max_v = maxs;
    s.mean = (n > 0) ? sum / n : 0.0;
    const double var = (n > 0) ? (sq / n - s.mean * s.mean) : 0.0;
    s.std = std::sqrt(var > 0.0 ? var : 0.0);
    return s;
}

// 从 PNG 构建物体 mask：非黑像素（背景 lin=0→srgb=0）。
inline std::vector<bool> BuildMaskFromPng(const std::string& path,
                                          int w, int h) {
    unsigned char* px = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    CHECK(px != nullptr) << "Failed to load for mask: " << path;
    std::vector<bool> mask(w * h, false);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* q = &px[i * 4];
        if (q[0] > 0 || q[1] > 0 || q[2] > 0) mask[i] = true;
    }
    stbi_image_free(px);
    return mask;
}

}  // namespace test
}  // namespace jpov

#endif  // JPOV_TEST_POSTPROCESS_FINAL_ADJUST_COMMON_H_

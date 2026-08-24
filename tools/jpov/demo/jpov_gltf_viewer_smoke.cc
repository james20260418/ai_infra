// JPOV glTF 查看器 — headless 冒烟渲染（开发自检用）
//
// 在无窗口环境下用 RunOnce 渲染一帧"默认视角"的场景，输出 PNG，用于
// 验证 task #3 验收（地平面 + glTF 模型 + 正午光照正确显示）而无需交互窗口。
//
// 用法：
//   xvfb-run -a ./bazel-bin/tools/jpov/jpov_gltf_viewer_smoke <gltf> <out.png>
//
// 与交互程序共用 view_config.h（ViewConfig/正午光照/地面），保证 self-check
// 视图即交互所见（架构 doc：交互与拍照共用 OneIteration + MakeNoonLighting）。

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/view_config.h"

namespace {

constexpr int kWidth  = 1280;
constexpr int kHeight = 720;

class SmokeApp : public JPOV {
public:
    using JPOV::JPOV;

    jpov::GltfObject gltf_;
    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    jpov_viewer::ViewConfig view_;
    jpov_viewer::NoonLighting noon_;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        cmds->camera.fbo_3d_width_  = kWidth;
        cmds->camera.fbo_3d_height_ = kHeight;
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        cmds->sky = noon_.sky;
        cmds->sun = noon_.sun;
        cmds->ambient = noon_.ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }
};

}  // namespace

int main(int argc, char** argv) {
    CHECK_GE(argc, 3) << "用法: jpov_gltf_viewer_smoke <gltf路径> <输出.png>";
    const std::string gltf_path = argv[1];
    const std::string out_png   = argv[2];

    JPOV::Config cfg;
    cfg.headless = true;  // 无可见窗口（自检）
    cfg.width  = kWidth;
    cfg.height = kHeight;
    SmokeApp app(cfg);
    app.Init();

    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty()) << "LoadGltf 失败: " << gltf_path;
    app.ground_mat_ = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad());
    app.noon_ = jpov_viewer::MakeNoonLighting();
    app.view_ = jpov_viewer::DefaultView();

    // 可选视角覆盖（调试/自检用）：JPOV_SMOKE_R / _PHI_DEG / _THETA_DEG。
    // 缺省用 DefaultView（(1,1,1)→(0,1,0)）。
    if (const char* r = std::getenv("JPOV_SMOKE_R")) {
        app.view_.R = std::atof(r);
    }
    if (const char* p = std::getenv("JPOV_SMOKE_PHI_DEG")) {
        app.view_.phi = std::atof(p) * (3.14159265358979323846 / 180.0);
    }
    if (const char* t = std::getenv("JPOV_SMOKE_THETA_DEG")) {
        app.view_.theta = std::atof(t) * (3.14159265358979323846 / 180.0);
    }

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    jpov::InputSnapshot input{};   // 默认视角（无交互输入）
    app.RunOnce(input, winfo, out_png.c_str());
    app.Finalize();
    LOG(INFO) << "smoke 渲染完成: " << out_png << " (" << kWidth << "x" << kHeight << ")";
    return 0;
}

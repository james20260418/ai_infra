// JPOV 模型编辑器 — headless 冒烟渲染（开发自检用，非交付产物）
//
// 无窗口环境下渲染"若干放置状态"的对比图，用于验证编辑器摆放链路（放置数学 →
// DrawGltfObject → 画面）确实生效、且排序/轴向符合预期，而无需交互窗口。
// 与交互程序共用 model_placement.h + view_config.h（同一份数学与场景），
// 造出的图即"人手拖动时会看到什么"。
//
// 用法：
//   xvfb-run -a ./bazel-bin/tools/jpov/jpov_model_editor_smoke <gltf> <out_dir>

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/editor_app.h"
#include "tools/jpov/demo/model_placement.h"
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
    jpov_viewer::ModelPlacement placement_;

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

        // 与编辑器同款固定光照 + 地面（0~-3 可调，冒烟用 -3）。
        const jpov_viewer::NoonLighting light = jpov_viewer::MakeLighting(
            jpov_viewer::kEditorSunElevDeg, jpov_viewer::kEditorTurbidity,
            jpov_viewer::kEditorSeasonR);
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);

        // 走与编辑器**完全相同**的路径：placement → ToDrawParams → DrawGltfObject。
        const jpov_viewer::DrawPlacement dp =
            jpov_viewer::ToDrawParams(placement_);
        cmds->DrawGltfObject(gltf_, dp.center, dp.up, dp.front, dp.scale);
    }
};

void RenderState(SmokeApp* app, const jpov_viewer::ModelPlacement& p,
                 const std::string& out_png) {
    app->placement_ = p;
    jpov::InputSnapshot input{};
    jpov::WindowInfo winfo;
    winfo.width = kWidth;
    winfo.height = kHeight;
    app->RunOnce(input, winfo, out_png.c_str());
    std::printf("%s\n", out_png.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    CHECK_EQ(argc, 3) << "用法: jpov_model_editor_smoke <gltf路径> <输出目录>";
    const std::string gltf_path = argv[1];
    const std::string out_dir   = argv[2];

    JPOV::Config cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.target_fps = 60;
    cfg.fonts = {
        {"tools/jpov/fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"tools/jpov/fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    SmokeApp app(cfg);
    app.Init();
    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty()) << "LoadGltf 失败: " << gltf_path;
    app.ground_mat_ = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad(-3.0f));
    app.view_ = jpov_viewer::DefaultView();
    if (app.gltf_.bounds_valid) {
        app.view_.R = jpov_viewer::ViewConfig::FitRadius(
            app.gltf_.bounds_min, app.gltf_.bounds_max, 60.0);
    }
    // 侧面视角（theta=90°）便于看清 X 轴旋转的效果。
    app.view_.phi = 0.0;
    app.view_.theta = 1.5707963;

    using jpov_viewer::ModelPlacement;
    // 1) 恒等（基线）。
    { ModelPlacement p; RenderState(&app, p, out_dir + "/smoke_00_identity.png"); }
    // 2) 仅缩放 2.0。
    { ModelPlacement p; p.scale = 2.0f;
      RenderState(&app, p, out_dir + "/smoke_01_scale2.png"); }
    // 3) 仅平移 tx=+0.05（该资产仅 ~18cm 长，图上看不出影响就说明链路没接上）。
    //    注：滑条范围 ±3m 是为人尺度资产定的；对这个小 demo 资产，按它自身尺度
    //    （~0.18m）取位移才有可见效果——这正是下面这条冒烟要验证的。
    { ModelPlacement p; p.tx = 0.05f;
      RenderState(&app, p, out_dir + "/smoke_02_tx.png"); }
    // 4) 仅绕 X 旋转 +45°。
    { ModelPlacement p; p.rx_deg = 45.0f;
      RenderState(&app, p, out_dir + "/smoke_03_rx45.png"); }
    // 5) 仅绕 Y 旋转 +45°。
    { ModelPlacement p; p.ry_deg = 45.0f;
      RenderState(&app, p, out_dir + "/smoke_04_ry45.png"); }
    // 6) 组合：缩放 2 + 平移 (0.05,0.03,0) + rx 30 + ry -60。
    { ModelPlacement p; p.scale = 2.0f; p.tx = 0.05f; p.ty = 0.03f;
      p.rx_deg = 30.0f; p.ry_deg = -60.0f;
      RenderState(&app, p, out_dir + "/smoke_05_combo.png"); }

    app.Finalize();
    return 0;
}

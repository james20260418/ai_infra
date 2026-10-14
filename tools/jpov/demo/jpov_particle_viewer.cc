// JPOV 粒子查看器（particle viewer）— 主程序
//
// 用途：验证「广告牌粒子 → WBO 半透明分级纹理（1/2）→ 合成 → froxel」。
// 场景 = 灰色地面 + 一张桌子 + 桌上的火与烟。火焰走新 ParticleRenderer（WBO 路径）。
//
// headless 拍摄：--capture <out_dir>
// 编译：./tools/jpov/build_particle_viewer.sh

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/particle_viewer_app.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/common/utils.h"

namespace {

void InstallScene(jpov_particle::ParticleViewerApp& app) {
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.ground_mat_ = jpov_skylight::GroundMaterial();
    app.table_top_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.45f, 0.04f, 0.80f));
    app.table_leg_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.04f, 0.35f, 0.04f));
    app.table_mat_ = jpov::PBRMaterial::SolidColor({0.35f, 0.22f, 0.12f, 1.0f});
}

void DefaultView(jpov_particle::ParticleViewerApp& app) {
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 0.22;
    app.view_.theta = 0.7;
    app.view_.R = 4.5;
    app.deg_.sun_elev_deg = 18.0f;
    app.deg_.turbidity = 2.0f;
}

int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV - particle viewer (capture)";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_particle::ParticleViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);

    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    DefaultView(app);
    app.fire_needs_reset_ = true;
    app.smoke_needs_reset_ = true;

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };
    for (int w = 0; w < 120; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_warm.png");
    }
    shoot("00_particle_0");
    shoot("00_particle_1");
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    shoot("99_panel");
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — 粒子查看器（particle viewer）";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = 30;
    cfg.headless = false;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_particle::ParticleViewerApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    InstallScene(app);
    DefaultView(app);
    app.Run();
    app.Finalize();
    return 0;
}

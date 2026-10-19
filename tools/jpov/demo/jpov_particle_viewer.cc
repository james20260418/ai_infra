// JPOV 粒子查看器（particle viewer）— 主程序
//
// 展示 7 种正式火焰 puff + 统一滑条（粒子数 / 时间快慢 / 燃料块 /
// 烟雾 / 温度增益 + 3 套温度→颜色映射）。
//
// headless 拍摄：--capture <out_dir> [--fire N] [--palette N]
// 编译：./tools/jpov/build_particle_viewer.sh

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
    // 矩形燃料块（单位 box，绘制时用 scale 拉成木块）
    app.fuel_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.9f, 0.12f, 0.45f));
    app.fuel_mat_ = jpov::PBRMaterial::SolidColor({0.18f, 0.10f, 0.06f, 1.0f});
}

void DefaultView(jpov_particle::ParticleViewerApp& app) {
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 0.18;
    app.view_.theta = 0.7;
    app.view_.R = 2.2;
    app.deg_.sun_elev_deg = 8.0f;
    app.deg_.turbidity = 2.0f;
}

int RunCapture(const std::string& out_dir, int fire_idx, int palette) {
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
    if (palette >= 0) { app.ui_palette_ = palette; }

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    const int first = (fire_idx >= 0) ? fire_idx : 0;
    const int last = (fire_idx >= 0) ? fire_idx
                                     : jpov_particle::kFireTexCount - 1;
    for (int i = first; i <= last; ++i) {
        app.fire_choice_ = i;
        app.fire_needs_reset_ = true;
        app.smoke_needs_reset_ = true;
        for (int w = 0; w < 90; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_warm.png");
        }
        char name[64];
        std::snprintf(name, sizeof(name), "fire_%02d", i);
        shoot(name);
        for (int w = 0; w < 6; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_gap.png");
        }
        std::snprintf(name, sizeof(name), "fire_%02d_b", i);
        shoot(name);
    }

    app.SetShowPanel(true);
    app.InstallTextMeasure();
    app.fire_choice_ = (fire_idx >= 0) ? fire_idx : 0;
    shoot("99_panel");
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    int fire_idx = -1;
    int palette = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--fire") == 0 && i + 1 < argc) {
            fire_idx = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--palette") == 0 && i + 1 < argc) {
            palette = std::atoi(argv[i + 1]);
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, fire_idx, palette);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — 粒子查看器（7 种正式火焰）";
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

// JPOV 粒子查看器（particle viewer）— 主程序
//
// 展示 7 种正式火焰 puff + 3 种烟 puff + 火星层 + 统一滑条（粒子数 / 时间快慢 /
// 燃料块 / 烟类型 / 烟雾照度 / 烟雾浓度 / 火星数量 / 火星大小 / 温度增益
// + 3 套温度→颜色映射）。
//
// headless 拍摄：--capture <out_dir> [--fire N] [--palette N]
//                [--smoke N]（逐个烟类型扫谱）[--smoke-density F]
//                [--ember-count N] [--ember-size F]
//                [--soft-depth F]（软深度过渡距离，单位米；F=0 = 关闭，便于 A/B）
//                [--fuel-y F] [--fuel-scale F]（燃料块位置/大小；软深度 A/B 用：
//                   fuel-y 调低可让火焰底部与地面相交，得到大面积的软过渡观察区）
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
    // 燃料块：单位立方体，绘制时用 scale（边长）缩放
    app.fuel_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(1.0f, 1.0f, 1.0f));
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

int RunCapture(const std::string& out_dir, int fire_idx, int palette,
               int smoke_idx, float count = -1.0f, float size = -1.0f,
               float smoke_density = -1.0f, float ember_count = -1.0f,
               float ember_size = -1.0f, float soft_depth = -1.0f,
               bool has_fuel_y = false, float fuel_y = 0.0f,
               bool has_fuel_scale = false, float fuel_scale = 0.0f) {
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
    if (count >= 0.0f) { app.ui_particle_count_ = count; }
    if (size > 0.0f) { app.ui_fire_size_ = size; }
    if (smoke_density >= 0.0f) { app.ui_smoke_density_ = smoke_density; }
    if (ember_count >= 0.0f) { app.ui_ember_count_ = ember_count; }
    if (ember_size > 0.0f) { app.ui_ember_size_ = ember_size; }
    if (has_fuel_y) { app.ui_fuel_y_ = fuel_y; }
    if (has_fuel_scale) { app.ui_fuel_scale_ = fuel_scale; }
    // 软深度：<0 = 用 App 默认值（开）；0 = 关闭（旧硬判据）；>0 = 开启并指定过渡距离。
    if (soft_depth == 0.0f) {
        app.ui_soft_depth_ = false;
    } else if (soft_depth > 0.0f) {
        app.ui_soft_depth_ = true;
        app.ui_soft_depth_m_ = soft_depth;
    }

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 烟类型扫谱：保持同一种火，逐个切换烟纹理，各拍一张。
    if (smoke_idx >= 0) {
        app.fire_choice_ = (fire_idx >= 0) ? fire_idx : 0;
        app.fire_needs_reset_ = true;
        for (int i = 0; i < jpov_particle::kSmokeTexCount; ++i) {
            app.smoke_choice_ = i;
            // 每种类型都从同一初始状态开始，保证 A/B 只差纹理。
            app.fire_needs_reset_ = true;
            app.smoke_needs_reset_ = true;
            app.sub_rng_ = jpov::particles::Rng(20261015u);
            for (int w = 0; w < 180; ++w) {
                app.RunOnce(input, winfo, "/tmp/_pv_warm.png");
            }
            char name[64];
            std::snprintf(name, sizeof(name), "smoke_%02d", i);
            shoot(name);
            for (int w = 0; w < 6; ++w) {
                app.RunOnce(input, winfo, "/tmp/_pv_gap.png");
            }
            std::snprintf(name, sizeof(name), "smoke_%02d_b", i);
            shoot(name);
        }
        app.SetShowPanel(true);
        app.InstallTextMeasure();
        shoot("99_panel");
        app.Finalize();
        return 0;
    }

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
    int smoke_idx = -1;
    float count = -1.0f, size = -1.0f, smoke_density = -1.0f;
    float ember_count = -1.0f, ember_size = -1.0f, soft_depth = -1.0f;
    bool has_fuel_y = false;
    bool has_fuel_scale = false;
    float fuel_y = 0.0f;
    float fuel_scale = 0.0f;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--fire") == 0 && i + 1 < argc) {
            fire_idx = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--palette") == 0 && i + 1 < argc) {
            palette = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--smoke") == 0 && i + 1 < argc) {
            smoke_idx = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--smoke-density") == 0 && i + 1 < argc) {
            smoke_density = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ember-count") == 0 && i + 1 < argc) {
            ember_count = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ember-size") == 0 && i + 1 < argc) {
            ember_size = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = std::atof(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--soft-depth") == 0 && i + 1 < argc) {
            soft_depth = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--fuel-y") == 0 && i + 1 < argc) {
            has_fuel_y = true;
            fuel_y = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--fuel-scale") == 0 && i + 1 < argc) {
            has_fuel_scale = true;
            fuel_scale = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = std::atof(argv[i + 1]);
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, fire_idx, palette, smoke_idx, count, size,
                          smoke_density, ember_count, ember_size,
                          /*soft_depth=*/soft_depth,
                          /*has_fuel_y=*/has_fuel_y, /*fuel_y=*/fuel_y,
                          /*has_fuel_scale=*/has_fuel_scale,
                          /*fuel_scale=*/fuel_scale);
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

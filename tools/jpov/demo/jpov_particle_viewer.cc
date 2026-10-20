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
//                [--align 0|1|2|3]（view/vertical/facing/blend）
//                [--fire-light 0|1] [--fire-light-intensity F]
//                [--bloom 0|1] [--bloom-intensity F] [--bloom-threshold F]
//                [--cam-phi-deg F]
//                （以上均用于 A/B：只有给了参数的项才覆盖 App 默认值）
// 编译：./tools/jpov/build_particle_viewer.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
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

// headless 拍摄参数。字段为 nullopt / 负数 = 不改 App 默认值。
// （参数已 15+ 个，位置参数太容易错传，收成结构体。）
struct CaptureOptions {
    std::string out_dir;

    // 扫谱/规模
    int fire_idx = -1;      // >=0 只拍这一种火
    int palette = -1;       // >=0 覆盖温度映射
    int smoke_idx = -1;     // >=0 逐个烟类型扫谱
    float count = -1.0f;    // >=0 粒子数
    float size = -1.0f;     // >0  火焰尺寸
    float smoke_density = -1.0f;
    float ember_count = -1.0f;
    float ember_size = -1.0f;

    // 场景
    std::optional<float> fuel_y;
    std::optional<float> fuel_scale;

    // buff 开关（A/B 用）
    std::optional<float> soft_depth;          // 0 = 关；>0 = 过渡距离（米）
    std::optional<int>   align_mode;      // 0=view 1=vertical 2=facing 3=blend
    std::optional<bool>  fire_light;
    std::optional<float> fire_light_intensity;
    std::optional<bool>  bloom;
    std::optional<float> bloom_intensity;
    std::optional<float> bloom_threshold;

    // 相机仰角（度）：用于「朝向自适应」等需要换视角才看得出的 A/B。
    std::optional<float> cam_phi_deg;
};

int RunCapture(const CaptureOptions& opts) {
    const std::string& out_dir = opts.out_dir;
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
    if (opts.cam_phi_deg.has_value()) {
        constexpr float kPi = 3.14159265358979323846f;
        app.view_.phi = static_cast<double>(*opts.cam_phi_deg * kPi / 180.0f);
    }
    if (opts.palette >= 0) { app.ui_palette_ = opts.palette; }
    if (opts.count >= 0.0f) { app.ui_particle_count_ = opts.count; }
    if (opts.size > 0.0f) { app.ui_fire_size_ = opts.size; }
    if (opts.smoke_density >= 0.0f) { app.ui_smoke_density_ = opts.smoke_density; }
    if (opts.ember_count >= 0.0f) { app.ui_ember_count_ = opts.ember_count; }
    if (opts.ember_size > 0.0f) { app.ui_ember_size_ = opts.ember_size; }
    if (opts.fuel_y.has_value()) { app.ui_fuel_y_ = *opts.fuel_y; }
    if (opts.fuel_scale.has_value()) { app.ui_fuel_scale_ = *opts.fuel_scale; }
    // 软深度：0 = 关闭（旧硬判据）；>0 = 开启并指定过渡距离。
    if (opts.soft_depth.has_value()) {
        app.ui_soft_depth_ = (*opts.soft_depth > 0.0f);
        if (*opts.soft_depth > 0.0f) {
            app.ui_soft_depth_m_ = *opts.soft_depth;
        }
    }
    if (opts.align_mode.has_value()) { app.ui_align_mode_ = *opts.align_mode; }
    if (opts.fire_light.has_value()) { app.ui_fire_light_ = *opts.fire_light; }
    if (opts.fire_light_intensity.has_value()) {
        app.ui_fire_light_intensity_ = *opts.fire_light_intensity;
    }
    if (opts.bloom.has_value()) { app.ui_bloom_ = *opts.bloom; }
    if (opts.bloom_intensity.has_value()) { app.ui_bloom_intensity_ = *opts.bloom_intensity; }
    if (opts.bloom_threshold.has_value()) { app.ui_bloom_threshold_ = *opts.bloom_threshold; }

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 烟类型扫谱：保持同一种火，逐个切换烟纹理，各拍一张。
    if (opts.smoke_idx >= 0) {
        app.fire_choice_ = (opts.fire_idx >= 0) ? opts.fire_idx : 0;
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

    const int first = (opts.fire_idx >= 0) ? opts.fire_idx : 0;
    const int last = (opts.fire_idx >= 0) ? opts.fire_idx
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
    app.fire_choice_ = (opts.fire_idx >= 0) ? opts.fire_idx : 0;
    shoot("99_panel");
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    CaptureOptions opts;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool has_val = (i + 1 < argc);
        if (std::strcmp(a, "--capture") == 0 && has_val) {
            opts.out_dir = argv[++i];
        } else if (std::strcmp(a, "--fire") == 0 && has_val) {
            opts.fire_idx = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--palette") == 0 && has_val) {
            opts.palette = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--smoke") == 0 && has_val) {
            opts.smoke_idx = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--smoke-density") == 0 && has_val) {
            opts.smoke_density = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--ember-count") == 0 && has_val) {
            opts.ember_count = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--ember-size") == 0 && has_val) {
            opts.ember_size = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--count") == 0 && has_val) {
            opts.count = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--size") == 0 && has_val) {
            opts.size = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--soft-depth") == 0 && has_val) {
            opts.soft_depth = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--fuel-y") == 0 && has_val) {
            opts.fuel_y = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--fuel-scale") == 0 && has_val) {
            opts.fuel_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--align") == 0 && has_val) {
            opts.align_mode = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--fire-light") == 0 && has_val) {
            opts.fire_light = (std::atoi(argv[++i]) != 0);
        } else if (std::strcmp(a, "--fire-light-intensity") == 0 && has_val) {
            opts.fire_light_intensity = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--bloom") == 0 && has_val) {
            opts.bloom = (std::atoi(argv[++i]) != 0);
        } else if (std::strcmp(a, "--bloom-intensity") == 0 && has_val) {
            opts.bloom_intensity = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--bloom-threshold") == 0 && has_val) {
            opts.bloom_threshold = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--cam-phi-deg") == 0 && has_val) {
            opts.cam_phi_deg = static_cast<float>(std::atof(argv[++i]));
        }
    }
    if (!opts.out_dir.empty()) {
        return RunCapture(opts);
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

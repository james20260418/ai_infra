// JPOV 粒子查看器（particle viewer）— 主程序
//
// 用途：验证「广告牌粒子 → WBO 半透明分级纹理（1/2）→ 合成 → froxel」。
// 场景 = 灰色地面 + 地面上的一团火与烟（无道具，便于观察粒子本身）。
// 火焰走 ParticleRenderer（WBO 路径），广告牌统一 kView。
//
// headless 拍摄：--capture <out_dir>
// 编译：./tools/jpov/build_particle_viewer.sh

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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
}

void DefaultView(jpov_particle::ParticleViewerApp& app) {
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 0.16;
    app.view_.theta = 0.72;
    app.view_.R = 1.8;      // 篝火尺度：贴近看
    app.deg_.sun_elev_deg = 7.0f;   // 低阳（黄昏侧光），让火更突出
    app.deg_.turbidity = 2.0f;
}

int RunSingleCapture(const std::string& out_dir, int idx, int frames,
                     bool envelope = false, bool v2_profile = false) {
    JPOV::Config cfg;
    cfg.title = "JPOV - single puff (capture)";
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
    app.hide_ground_ = true;   // 单团观察：干净黑底，避免地平线遮挡/切边
    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    DefaultView(app);
    // 单团观察：压暗天空 + 贴近 + 放大，才看得清贴图细节
    app.deg_.sun_elev_deg = -8.0f;
    app.deg_.night_blue = 0.35f;
    app.view_.R = 2.4;
    app.ui_fire_size_ = 0.85f;
    app.fire_enable_ = false;
    app.smoke_enable_ = false;
    app.sparks_enable_ = false;
    app.single_puff_ = true;
    app.single_envelope_ = envelope;  // 默认贴图原色；掩膜素材需叠色带
    app.v2_profile_ = v2_profile;
    app.fire_choice_ = idx;
    app.InstallTextMeasure();

    for (int w = 0; w < 10; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_single_warm.png");
    }
    // 精确覆盖一个寿命：每张直接把 clock 设到 t*life（不受整数帧步进误差影响）
    const float life = 0.5f * (app.fire_cfg_.lifetime.min + app.fire_cfg_.lifetime.max);
    LOG(INFO) << "[single] life=" << life << "s frames=" << frames;
    for (int i = 0; i < frames; ++i) {
        const float t = (frames > 1) ? (static_cast<float>(i) / (frames - 1)) : 0.0f;
        app.SetClock(std::max(0.0f, t * life - 1.0f / 60.0f));
        const std::string path =
            out_dir + "/single_" + std::to_string(i) + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
        for (int w = 0; w < 1; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_single_gap.png");
        }
    }
    app.Finalize();
    return 0;
}

int RunFullCapture(const std::string& out_dir, int idx, int frames,
                    bool v2_profile = false) {
    JPOV::Config cfg;
    cfg.title = "JPOV - full particles (capture)";
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
    app.deg_.sun_elev_deg = -8.0f;
    app.deg_.night_blue = 0.35f;
    app.fire_choice_ = idx;
    app.v2_profile_ = v2_profile;
    // FS1 是一整团大形状：默认 0.22m 会糊成一坨，拍摄时用小一点、密一点
    if (idx == 7) {
        app.ui_fire_size_ = 0.11f;
        app.ui_fire_rate_ = 420.0f;
    }
    app.fire_enable_ = true;
    app.smoke_enable_ = false;
    app.sparks_enable_ = false;
    app.fire_needs_reset_ = true;
    for (int w = 0; w < 90; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_full_warm.png");
    }
    for (int i = 0; i < frames; ++i) {
        const std::string path = out_dir + "/full_" + std::to_string(i) + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
        for (int w = 0; w < 10; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_full_gap.png");
        }
    }
    app.Finalize();
    return 0;
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
    app.sparks_needs_reset_ = true;

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };
    for (int w = 0; w < 120; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_warm.png");
    }
    // 每隔 8 帧拍一张（≈0.27s）：拼起来能看出火的窜动/烟的上浮，而不是同一瞬间。
    shoot("00_particle_0");
    for (int w = 0; w < 8; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_gap.png");
    }
    shoot("00_particle_1");
    for (int w = 0; w < 8; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_gap.png");
    }
    shoot("00_particle_2");

    // 火星专项图：只留火星层（kVelocity + stretched），便于看清「拉丝是否沿运动方向」。
    app.sparks_enable_ = true;
    app.fire_enable_ = false;
    app.smoke_enable_ = false;
    for (int w = 0; w < 30; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_warm_spark.png");
    }
    shoot("10_sparks_only");
    app.fire_enable_ = true;
    app.smoke_enable_ = true;

    // 单 puff 观察模式：沿循环拍 4 张（每张间隔 1/4 个 sheet 周期）。
    app.single_puff_ = true;
    app.fire_enable_ = false;
    app.smoke_enable_ = false;
    app.sparks_enable_ = false;
    for (int i = 0; i < 7; ++i) {
        const std::string name = "20_puff_phase" + std::to_string(i);
        shoot(name.c_str());
        for (int w = 0; w < 8; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_puff_gap.png");
        }
    }
    // 自产火团 v4 的观察图
    app.fire_choice_ = 1;
    for (int w = 0; w < 8; ++w) {
        app.RunOnce(input, winfo, "/tmp/_pv_v4gap.png");
    }
    shoot("21_puff_v4");
    // 三个 Astralax 法 fire flipbook 的观察图
    for (int k = 0; k < 3; ++k) {
        app.fire_choice_ = 2 + k;   // 2 = MP 篝火 / 3 = 火把 / 4 = 主火
        for (int w = 0; w < 8; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_mpgap.png");
        }
        const std::string nm = "30_mp" + std::to_string(k);
        shoot(nm.c_str());
    }
    // 形变模式（第 4 个）：单 puff 观察，看 F1↔F3↔F5 的渐变
    app.fire_choice_ = 3;
    for (int i = 0; i < 4; ++i) {
        const std::string name = "40_morph" + std::to_string(i);
        shoot(name.c_str());
        for (int w = 0; w < 10; ++w) {
            app.RunOnce(input, winfo, "/tmp/_pv_morphgap.png");
        }
    }
    app.fire_choice_ = 0;
    app.single_puff_ = false;
    app.fire_enable_ = true;
    app.smoke_enable_ = true;

    app.SetShowPanel(true);
    app.InstallTextMeasure();
    shoot("99_panel");
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string single_dir;
    std::string full_dir;
    int full_idx = -1;
    int single_idx = -1;
    int single_frames = 8;
    bool single_envelope = false;
    bool v2_profile = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--single") == 0 && i + 1 < argc) {
            single_idx = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--single-frames") == 0 && i + 1 < argc) {
            single_frames = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--single-dir") == 0 && i + 1 < argc) {
            single_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--single-envelope") == 0) {
            single_envelope = true;
        } else if (std::strcmp(argv[i], "--v2-profile") == 0) {
            v2_profile = true;
        } else if (std::strcmp(argv[i], "--full") == 0 && i + 1 < argc) {
            full_idx = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--full-dir") == 0 && i + 1 < argc) {
            full_dir = argv[i + 1];
        }
    }
    if (full_idx >= 0) {
        if (full_dir.empty()) full_dir = "/tmp";
        return RunFullCapture(full_dir, full_idx, single_frames, v2_profile);
    }
    if (single_idx >= 0) {
        if (single_dir.empty()) single_dir = "/tmp";
        return RunSingleCapture(single_dir, single_idx, single_frames, single_envelope, v2_profile);
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

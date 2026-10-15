// JPOV 体积火查看器 — 主程序
//
// 场景 = 灰色地面 + 1m³ 燃料块 + 块顶的体积火（逐像素 raymarch → WBO 薄层合成）。
// headless 拍摄：--capture <out_dir> [--volume <path.jpovvol>]
// 编译：./tools/jpov/build_volume_fire_viewer.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/volume_fire_viewer_app.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/common/utils.h"

namespace {

#ifndef _WIN32
#include <unistd.h>
std::string ExeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const std::string p(buf);
        const size_t s = p.find_last_of('/');
        if (s != std::string::npos) { return p.substr(0, s); }
    }
    return std::string();
}
#else
std::string ExeDir() { return std::string(); }
#endif

std::string ResolveVolumePath(const std::string& arg) {
    if (!arg.empty()) {
        return arg;
    }
    const std::string d = ExeDir();
    const char* rels[] = {"volumes/fire_stable.jpovvol", "fire_stable.jpovvol",
                          "../data/fire_vdb/fire_stable.jpovvol"};
    for (const char* r : rels) {
        const std::string p = d.empty() ? std::string(r) : (d + "/" + r);
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f != nullptr) {
            std::fclose(f);
            return p;
        }
    }
    return std::string();
}

void InstallScene(jpov_volume_fire::VolumeFireViewerApp& app) {
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.ground_mat_ = jpov_skylight::GroundMaterial();
    // 1m³ 燃料块（半宽 0.5）。
    app.fuel_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 0.5f, 0.5f));
    app.fuel_mat_ = jpov::PBRMaterial::SolidColor({0.05f, 0.035f, 0.03f, 1.0f});
}

void DefaultView(jpov_volume_fire::VolumeFireViewerApp& app) {
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 0.22f;
    app.view_.theta = 0.7f;
    app.view_.R = 8.0f;
    app.deg_.sun_elev_deg = 18.0f;
    app.deg_.turbidity = 2.0f;
}

int RunCapture(const std::string& out_dir, const std::string& volume_path,
               const std::string& dump_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV - volume fire viewer (capture)";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_volume_fire::VolumeFireViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    app.volume_path_ = volume_path;
    if (!dump_dir.empty()) {
        app.SetVolumeDebugDump(dump_dir);
    }

    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    DefaultView(app);

    auto shoot = [&](const char* name) {
        LOG(INFO) << "[camera] " << name << " | " << app.CamString();
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };
    for (int w = 0; w < 30; ++w) {
        app.RunOnce(input, winfo, "/tmp/_vf_warm.png");
    }
    // 不同播放时刻。
    for (int i = 0; i < 6; ++i) {
        char nm[64];
        std::snprintf(nm, sizeof(nm), "10_frame_%02d", i);
        shoot(nm);
    }
    // σ / 发射 对比。
    app.sigma_scale_ = 2.0f;
    shoot("20_sigma2");
    app.sigma_scale_ = 8.0f;
    shoot("21_sigma8");
    app.sigma_scale_ = 4.0f;
    app.emission_scale_ = 2.0f;
    shoot("22_emis2");
    app.emission_scale_ = 1.0f;
    // 抖动开/关。
    app.jitter_ = false;
    shoot("30_nojit");
    app.jitter_ = true;
    // 撒掉燃料块（排查伪影是否来自方块）。
    app.fuel_enable_ = false;
    shoot("40_nocube");
    app.fuel_enable_ = true;
    // 全屏 quad（去掉盒光栅化）——二分定位。
    app.fs_debug_ = true;
    shoot("50_fullscreen");
    app.fs_debug_ = false;
    // 面板。
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    shoot("99_panel");
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string volume_arg;
    std::string dump_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            volume_arg = argv[i + 1];
        } else if (std::strcmp(argv[i], "--dump-volume") == 0 && i + 1 < argc) {
            dump_dir = argv[i + 1];
        }
    }
    const std::string volume_path = ResolveVolumePath(volume_arg);
    if (volume_path.empty()) {
        LOG(WARNING) << "找不到 .jpovvol 体积文件（用 --volume 指定）";
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, volume_path, dump_dir);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — 体积火查看器（volume fire viewer）";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = 30;
    cfg.headless = false;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_volume_fire::VolumeFireViewerApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    InstallScene(app);
    app.volume_path_ = volume_path;
    DefaultView(app);
    app.Run();
    app.Finalize();
    return 0;
}

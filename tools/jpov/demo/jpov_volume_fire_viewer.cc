// JPOV 体积火查看器 — 主程序
//
// 场景 = 灰色地面 + 1m³ 燃料块 + 块顶的体积火（逐像素 raymarch → WBO 薄层合成）。
// headless 拍摄：--capture <out_dir> [--volume <path.jpovvol>]
// A/B 基准拍摄：--ab-capture <out.png> [--volume <path.jpovvol>] [--ab-frame N]
//               [--ab-box X | X,Y,Z] [--ab-y-shift M] [--ab-sigma S] [--ab-emis E]
//               [--ab-smoke K] [--ab-smoke-sigma N] [--ab-light r,g,b]
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

// A/B 基准的旋钮（默认 = 查看器的可复现默认；CLI 可覆盖，便于扫参/复现）。
struct AbParams {
    int frame = 0;
    jpov::Vec3f size = {1.22f, 1.22f, 1.22f};  // 火盒各轴尺寸（米）= 资产 dims×voxel_m
    float y_shift = -0.10f;                    // 取景下移（米，与参考基线对齐）
    float sigma = jpov_volume_fire::VolumeFireViewerApp::kDefaultSigma;
    float emission = jpov_volume_fire::VolumeFireViewerApp::kDefaultEmission;
    float temp = jpov_volume_fire::VolumeFireViewerApp::kDefaultTempKelvin;
    float temp_min = jpov_volume_fire::VolumeFireViewerApp::kDefaultTempMin;
    float temp_pow = jpov_volume_fire::VolumeFireViewerApp::kDefaultTempPow;
    float temp_ref = jpov_volume_fire::VolumeFireViewerApp::kDefaultTempRef;
    std::string palette = jpov_volume_fire::VolumeFireViewerApp::DefaultPalette();
    float ramp_lo = jpov_volume_fire::VolumeFireViewerApp::kDefaultRampLo;
    float ramp_hi = jpov_volume_fire::VolumeFireViewerApp::kDefaultRampHi;
    bool bloom = false;
    float bloom_intensity = 0.6f;
    float bloom_threshold = 1.0f;
    float sat = jpov_volume_fire::VolumeFireViewerApp::kDefaultColorSat;
    float smoke_pow = 1.0f;
    float smoke = jpov_volume_fire::VolumeFireViewerApp::kDefaultSmokeScatter;
    float smoke_sigma = jpov_volume_fire::VolumeFireViewerApp::kDefaultSmokeSigma;
    jpov::Vec3f light = jpov_volume_fire::VolumeFireViewerApp::DefaultSmokeLight();
};

// A/B 基准拍摄（P0）：产出与 EmberGen 官方渲染可直接比对的**孤立火**图。
//   - 钉相机：正面（phi=0/theta=0，+Z 看向原点），R 由火盒自动取景（FitRadius）。
//   - 钉帧：asset 帧下标固定为 p.frame（可复现；与官方视频帧的对应见 docs 计划文档）。
//   - 纯黑背景 + 无地面/燃料块（只留火本体）。
// 输出：out_png（完整文件路径，目录需已存在）。
int RunAbCapture(const std::string& out_png, const std::string& volume_path,
                 const AbParams& p) {
    CHECK_GE(p.frame, 0) << "--ab-frame 必须 >= 0";
    CHECK_GT(p.size.x(), 0.0f) << "--ab-box X 必须 > 0";
    CHECK_GT(p.size.y(), 0.0f) << "--ab-box Y 必须 > 0";
    CHECK_GT(p.size.z(), 0.0f) << "--ab-box Z 必须 > 0";

    JPOV::Config cfg;
    cfg.title = "JPOV - volume fire viewer (A/B baseline)";
    cfg.width = jpov_viewer::kViewerWidth;
    cfg.height = jpov_viewer::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_volume_fire::VolumeFireViewerApp app(cfg);
    app.SetShowPanel(false);
    app.SetShowCamInfo(false);
    app.Init();
    InstallScene(app);
    app.volume_path_ = volume_path;
    app.sigma_scale_ = p.sigma;
    app.emission_scale_ = p.emission;
    app.temp_kelvin_scale_ = p.temp;
    app.temp_kelvin_min_ = p.temp_min;
    app.temp_emission_pow_ = p.temp_pow;
    app.temp_kelvin_ref_ = p.temp_ref;
    app.palette_path_ = p.palette;
    app.palette_ramp_lo_ = p.ramp_lo;
    app.palette_ramp_hi_ = p.ramp_hi;
    app.bloom_enabled_ = p.bloom;
    app.bloom_intensity_ = p.bloom_intensity;
    app.bloom_threshold_ = p.bloom_threshold;
    app.sat_ = p.sat;
    app.smoke_pow_ = p.smoke_pow;
    app.smoke_scatter_ = p.smoke;
    app.smoke_sigma_scale_ = p.smoke_sigma;
    app.smoke_light_color_ = p.light;

    // 钉相机：正面看向原点，距离由火盒（各轴真实尺寸 + 取景下移）自动取景。
    const float bmin[3] = {-0.5f * p.size.x(), -0.5f * p.size.y() + p.y_shift,
                           -0.5f * p.size.z()};
    const float bmax[3] = {0.5f * p.size.x(), 0.5f * p.size.y() + p.y_shift,
                           0.5f * p.size.z()};
    app.view_ = jpov_viewer::ViewConfig{};
    app.view_.phi = 0.0;
    app.view_.theta = 0.0;
    app.view_.R = jpov_viewer::ViewConfig::FitRadius(bmin, bmax, /*fov_deg=*/60.0);

    app.ab_mode_ = true;
    app.ab_frame_ = p.frame;
    app.ab_size_ = p.size;
    app.ab_y_shift_ = p.y_shift;

    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    // 预热：让首帧的 GL 资源就绪（相机/帧都固定，无状态演化）。
    for (int w = 0; w < 30; ++w) {
        app.RunOnce(input, winfo, "/tmp/_vf_ab_warm.png");
    }
    LOG(INFO) << "[ab] " << app.CamString();
    app.RunOnce(input, winfo, out_png.c_str());
    LOG(INFO) << "ab capture: " << out_png;
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string ab_out;
    std::string volume_arg;
    std::string dump_dir;
    AbParams ab;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ab-capture") == 0 && i + 1 < argc) {
            ab_out = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ab-frame") == 0 && i + 1 < argc) {
            ab.frame = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--ab-box") == 0 && i + 1 < argc) {
            // "X" = 立方盒；"X,Y,Z" = 各轴独立（推荐：直接给资产真实尺寸，避免拉伸）。
            float bx = 0.0f, by = 0.0f, bz = 0.0f;
            if (std::sscanf(argv[i + 1], "%f,%f,%f", &bx, &by, &bz) == 3) {
                ab.size = {bx, by, bz};
            } else if (std::sscanf(argv[i + 1], "%f", &bx) == 1) {
                ab.size = {bx, bx, bx};
            } else {
                LOG(WARNING) << "--ab-box 需要 \"X\" 或 \"X,Y,Z\"，已忽略: " << argv[i + 1];
            }
        } else if (std::strcmp(argv[i], "--ab-y-shift") == 0 && i + 1 < argc) {
            ab.y_shift = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-sigma") == 0 && i + 1 < argc) {
            ab.sigma = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-emis") == 0 && i + 1 < argc) {
            ab.emission = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-temp") == 0 && i + 1 < argc) {
            ab.temp = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-temp-min") == 0 && i + 1 < argc) {
            ab.temp_min = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-temp-pow") == 0 && i + 1 < argc) {
            ab.temp_pow = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-temp-ref") == 0 && i + 1 < argc) {
            ab.temp_ref = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-palette") == 0 && i + 1 < argc) {
            ab.palette = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ab-ramp-lo") == 0 && i + 1 < argc) {
            ab.ramp_lo = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-ramp-hi") == 0 && i + 1 < argc) {
            ab.ramp_hi = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-bloom") == 0 && i + 1 < argc) {
            ab.bloom = std::atoi(argv[i + 1]) != 0;
        } else if (std::strcmp(argv[i], "--ab-bloom-intensity") == 0 && i + 1 < argc) {
            ab.bloom_intensity = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-bloom-threshold") == 0 && i + 1 < argc) {
            ab.bloom_threshold = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-sat") == 0 && i + 1 < argc) {
            ab.sat = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-smoke-pow") == 0 && i + 1 < argc) {
            ab.smoke_pow = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-smoke") == 0 && i + 1 < argc) {
            ab.smoke = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-smoke-sigma") == 0 && i + 1 < argc) {
            ab.smoke_sigma = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--ab-light") == 0 && i + 1 < argc) {
            float lr = 0.0f, lg = 0.0f, lb = 0.0f;
            if (std::sscanf(argv[i + 1], "%f,%f,%f", &lr, &lg, &lb) == 3) {
                ab.light = {lr, lg, lb};
            } else {
                LOG(WARNING) << "--ab-light 需要 \"r,g,b\"，已忽略: " << argv[i + 1];
            }
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
    if (!ab_out.empty()) {
        return RunAbCapture(ab_out, volume_path, ab);
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

// JPOV 体积火查看器 — 主程序
//
// 场景 = 灰色地面 + 1m³ 燃料块 + 块顶的体积火（逐像素 raymarch → WBO 薄层合成）。
// headless 拍摄：--capture <out_dir> [--volume <path.jpovvol>] [--prototypes-dir <dir>]
// 编译：./tools/jpov/build_volume_fire_viewer.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

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

// 四大原型族（文件名 = 族名）。
const char* const kProtoFamilies[] = {"jet", "area", "pool", "transient"};
const int kProtoFamilyCount = 4;

// 解析原型族目录：--prototypes-dir 优先；否则在 exe 旁 / 数据目录里找含 jet.jpovvol 的目录。
std::string ResolveProtoDir(const std::string& arg) {
    std::vector<std::string> cands;
    if (!arg.empty()) { cands.push_back(arg); }
    const std::string d = ExeDir();
    const char* rels[] = {"volumes/prototypes", "prototypes",
                          "../data/fire_vdb/prototypes", "data/fire_vdb/prototypes"};
    for (const char* r : rels) {
        cands.push_back(d.empty() ? std::string(r) : (d + "/" + r));
    }
    for (const std::string& c : cands) {
        std::FILE* f = std::fopen((c + "/jet.jpovvol").c_str(), "rb");
        if (f != nullptr) { std::fclose(f); return c; }
    }
    return std::string();
}

// 读 JPVOL2 头里的 nframes（offset 24，uint32 LE）。
int JpovvolFrames(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) { return 0; }
    unsigned char h[40];
    const size_t n = std::fread(h, 1, sizeof(h), f);
    std::fclose(f);
    if (n < 40) { return 0; }
    return static_cast<int>(h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24));
}

// 简易解析 sidecar：找 "key": [a, b]。
bool ParseRangePair(const std::string& s, const std::string& key, int* a, int* b) {
    const std::string k = "\"" + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) { return false; }
    p = s.find('[', p);
    if (p == std::string::npos) { return false; }
    const size_t q = s.find(']', p);
    if (q == std::string::npos) { return false; }
    const std::string inner = s.substr(p + 1, q - p - 1);
    int va = 0, vb = 0;
    if (std::sscanf(inner.c_str(), "%d , %d", &va, &vb) != 2 &&
        std::sscanf(inner.c_str(), "%d,%d", &va, &vb) != 2) { return false; }
    *a = va; *b = vb;
    return true;
}

// 读 <vol>.json 里的三段帧窗；缺省 = 整段 [0,nf]。
bool ParseFloatVal(const std::string& s, const std::string& key, float* v) {
    const std::string k = "\"" + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) { return false; }
    p = s.find(':', p);
    if (p == std::string::npos) { return false; }
    *v = static_cast<float>(std::atof(s.c_str() + p + 1));
    return true;
}

void LoadProtoSegments(const std::string& vol_path, int nf,
                       std::pair<int, int>* ig, std::pair<int, int>* su,
                       std::pair<int, int>* ex, float* voxel_m) {
    *ig = {0, nf}; *su = {0, nf}; *ex = {0, nf};
    *voxel_m = 0.0f;
    std::ifstream f(vol_path + ".json", std::ios::binary);
    if (!f) { return; }
    const std::string s((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    int a = 0, b = 0;
    if (ParseRangePair(s, "ignite", &a, &b)) { *ig = {a, b}; }
    if (ParseRangePair(s, "sustain", &a, &b)) { *su = {a, b}; }
    if (ParseRangePair(s, "extinct", &a, &b)) { *ex = {a, b}; }
    ParseFloatVal(s, "voxel_m", voxel_m);
}

// 扫描原型目录，按固定族序装载存在的 .jpovvol + 分段帧窗。
void LoadPrototypes(const std::string& dir, std::vector<std::string>* paths,
                    std::vector<std::string>* names,
                    std::vector<std::pair<int, int>>* ig,
                    std::vector<std::pair<int, int>>* su,
                    std::vector<std::pair<int, int>>* ex,
                    std::vector<float>* voxel_m) {
    for (int i = 0; i < kProtoFamilyCount; ++i) {
        const std::string p = dir + "/" + kProtoFamilies[i] + ".jpovvol";
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f == nullptr) { continue; }
        std::fclose(f);
        names->push_back(kProtoFamilies[i]);
        paths->push_back(p);
        const int nf = JpovvolFrames(p);
        std::pair<int, int> a{0, nf}, b{0, nf}, c{0, nf};
        float vm = 0.0f;
        LoadProtoSegments(p, nf, &a, &b, &c, &vm);
        ig->push_back(a); su->push_back(b); ex->push_back(c); voxel_m->push_back(vm);
    }
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
               const std::string& dump_dir, const std::string& proto_dir) {
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

    // 原型族：装载（若目录可用）。
    std::vector<std::string> proto_paths, proto_names;
    std::vector<std::pair<int, int>> pig, psu, pex;
    std::vector<float> pvm;
    if (!proto_dir.empty()) { LoadPrototypes(proto_dir, &proto_paths, &proto_names, &pig, &psu, &pex, &pvm); }
    app.proto_paths_ = proto_paths;
    app.proto_names_ = proto_names;
    app.proto_ig_ = pig;
    app.proto_su_ = psu;
    app.proto_ex_ = pex;
    app.proto_voxel_m_ = pvm;

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

    // 原型火纹理查看模式：每族 × 三种播放模式（循环/点火/熄灭）各一张。
    if (!proto_names.empty()) {
        app.proto_mode_ = true;
        for (int i = 0; i < static_cast<int>(proto_names.size()); ++i) {
            for (int m = 0; m < 3; ++m) {
                app.proto_index_ = i;
                app.proto_play_mode_ = m;
                for (int w = 0; w < 12; ++w) { app.RunOnce(input, winfo, "/tmp/_vf_warm.png"); }
                const float tf = (m == 0) ? 45.0f : (m == 1) ? 15.0f : 73.0f;
                app.play_time_ = tf / app.fps_;
                char nm[80];
                std::snprintf(nm, sizeof(nm), "70_proto_%s_m%d", proto_names[i].c_str(), m);
                shoot(nm);
            }
        }
    }
    app.Finalize();
    return 0;
}

// 持续段逐帧输出（供拼 GIF 检查循环连续性）。
int RunLoopDump(const std::string& out_dir, const std::string& proto_dir,
                const std::string& family) {
    JPOV::Config cfg;
    cfg.title = "loop dump";
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
    std::vector<std::string> proto_paths, proto_names;
    std::vector<std::pair<int, int>> pig, psu, pex;
    std::vector<float> pvm;
    LoadPrototypes(proto_dir, &proto_paths, &proto_names, &pig, &psu, &pex, &pvm);
    app.proto_paths_ = proto_paths;
    app.proto_names_ = proto_names;
    app.proto_ig_ = pig; app.proto_su_ = psu; app.proto_ex_ = pex;
    app.proto_voxel_m_ = pvm;
    jpov::WindowInfo winfo;
    winfo.width = jpov_viewer::kViewerWidth;
    winfo.height = jpov_viewer::kViewerHeight;
    jpov::InputSnapshot input{};
    DefaultView(app);
    int idx = 0;
    for (int i = 0; i < static_cast<int>(proto_names.size()); ++i) {
        if (proto_names[i] == family) { idx = i; }
    }
    app.proto_mode_ = true;
    app.proto_index_ = idx;
    app.proto_play_mode_ = 0;   // 循环
    for (int w = 0; w < 10; ++w) { app.RunOnce(input, winfo, "/tmp/_vf_warm.png"); }
    const int su0 = app.proto_su_[static_cast<size_t>(idx)].first;
    const int su1 = app.proto_su_[static_cast<size_t>(idx)].second;
    for (int f = su0; f < su1; ++f) {
        app.play_time_ = static_cast<float>(f - su0) / app.fps_;
        char nm[160];
        std::snprintf(nm, sizeof(nm), "%s/loop_%03d.png", out_dir.c_str(), f - su0);
        app.RunOnce(input, winfo, nm);
    }
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string volume_arg;
    std::string dump_dir;
    std::string proto_dir_arg;
    std::string loop_dump_dir;
    std::string loop_family = "jet";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            volume_arg = argv[i + 1];
        } else if (std::strcmp(argv[i], "--dump-volume") == 0 && i + 1 < argc) {
            dump_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--prototypes-dir") == 0 && i + 1 < argc) {
            proto_dir_arg = argv[i + 1];
        } else if (std::strcmp(argv[i], "--loop-dump") == 0 && i + 1 < argc) {
            loop_dump_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--loop-family") == 0 && i + 1 < argc) {
            loop_family = argv[i + 1];
        }
    }
    const std::string volume_path = ResolveVolumePath(volume_arg);
    const std::string proto_dir = ResolveProtoDir(proto_dir_arg);
    if (proto_dir.empty()) {
        LOG(WARNING) << "未找到原型族目录（用 --prototypes-dir 指定；需 jet/area/pool/transient.jpovvol）";
    } else {
        LOG(INFO) << "prototypes dir = " << proto_dir;
    }
    if (volume_path.empty()) {
        LOG(WARNING) << "找不到 .jpovvol 体积文件（用 --volume 指定）";
    }
    if (!loop_dump_dir.empty()) {
        return RunLoopDump(loop_dump_dir, proto_dir, loop_family);
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, volume_path, dump_dir, proto_dir);
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
    {
        std::vector<std::string> proto_paths, proto_names;
        std::vector<std::pair<int, int>> pig, psu, pex;
        std::vector<float> pvm;
        if (!proto_dir.empty()) { LoadPrototypes(proto_dir, &proto_paths, &proto_names, &pig, &psu, &pex, &pvm); }
        app.proto_paths_ = proto_paths;
        app.proto_names_ = proto_names;
        app.proto_ig_ = pig;
        app.proto_su_ = psu;
        app.proto_ex_ = pex;
        app.proto_voxel_m_ = pvm;
    }
    DefaultView(app);
    app.Run();
    app.Finalize();
    return 0;
}

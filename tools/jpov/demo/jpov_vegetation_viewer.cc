// JPOV 植被查看器（vegetation viewer）— 主程序
//
// 用途：**植物生成链路的最后环节工具** —— 装载一个已生成的植物 GLB，在
// **标准天光（昼夜可调）** 下肉眼观察 + 出图。植物资产路径由 `--glb <path>` 指定
// （默认装载随工具分发的柳树范本）。
//
// 交互面板（天光自由度取自 skylight_scene，口径与天光查看器一致）：
//   浊度 / 季节色温 日光 / 天体仰角 / 天体方位角 / 月色变红 / 夜空偏蓝 /
//   三色环境光开关 / **SMAA 抗锯齿（默认开）**。
//
// headless 拍摄（无窗口，供交付验收 / 自动化核对）：
//   --capture <out_dir>        昼夜扫谱（天体仰角 +60° → −60°）+ 正午/夜间两个机位
//   --capture-smaa <out_dir>   固定光照，只切 SMAA 开关（smaa_off / smaa_on）
//   --capture-ui <out_dir>     面板可见的单帧截图（供验证右下角面板布局）
//
// 架构：与 skylight viewer 同构（主程序只做装配 + 事件循环；场景/天光装配在
// skylight_scene.h，渲染核心 + 面板在 vegetation_viewer_app.h）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   ./tools/jpov/build_jpov_vegetation_viewer.sh
//   → output/jpov_vegetation_viewer/jpov_vegetation_viewer [--glb <path>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/vegetation_mesh_export.h"
#include "tools/jpov/demo/vegetation_viewer_app.h"
#include "tools/jpov/demo/view_config.h"

namespace {

using jpov_vegetation::VegetationViewerApp;

constexpr int kWidth  = jpov_vegetation::kViewerWidth;
constexpr int kHeight = jpov_vegetation::kViewerHeight;
constexpr double kPi  = 3.14159265358979323846;

// 交互窗口 / 拍摄共用的字体声明（路径相对 exe 的 fonts/，build 脚本同构拷贝）。
std::vector<JPOV::FontEntry> ViewerFonts() {
    return {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",          0, jpov::kFontBuiltinLatin},
    };
}

// 解析要装载的植物 GLB 路径：
//   --glb 显式给出 → 原样走 ResolveResourcePath（支持绝对路径 / 相对 exe / 相对 cwd）；
//   否则 → 默认名经 ResolvePlantPath 在（exe 旁 plants/ → 源码 samples/）里找。
std::string ResolveGlbArg(const std::string& glb_arg) {
    if (!glb_arg.empty()) {
        return jpov::ResolveResourcePath(glb_arg);
    }
    return jpov_vegetation::ResolvePlantPath(
        jpov_vegetation::kDefaultPlantFile);
}

// headless 拍摄：昼夜扫谱 + 正午/夜间两个机位，写到 out_dir。
int RunCapture(const std::string& out_dir, const std::string& glb_path) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被查看器（headless capture）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(false);   // 纯 3D 截图
    app.Init();
    app.InstallSceneWithGltf(glb_path);

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    auto shoot = [&](const std::string& name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // ── A. 昼夜扫谱：同一条「天体仰角」轴，从正午扫到午夜（>0 段主光=太阳、
    //    <0 段主光=月亮），验证植物受光随昼夜平滑过渡。──
    app.deg_ = jpov_skylight::SkyDegrees{};
    app.deg_.sun_azim_deg = 45.0f;
    for (int e : {60, 35, 10, -10, -30, -60}) {
        app.deg_.sun_elev_deg = static_cast<float>(e);
        const std::string tag = (e >= 0) ? ("p" + std::to_string(e))
                                         : ("m" + std::to_string(-e));
        shoot("daynight_" + tag);
    }

    // ── B. 拍摄机位：正午与夜间各出「正面 / 3-4 侧」两张（出片用）。──
    struct ViewAngle { const char* name; float phi; float theta; };
    const ViewAngle kAngles[] = {
        {"front", 0.08f, 0.0f},                        // 正面
        {"hero",  0.12f, static_cast<float>(kPi / 4.0)},  // 3/4 侧（默认观察角）
    };
    const float kBaseR = static_cast<float>(app.view_.R);
    for (int pass = 0; pass < 2; ++pass) {
        app.deg_.sun_elev_deg = (pass == 0) ? 45.0f : -30.0f;
        const char* phase = (pass == 0) ? "noon" : "night";
        for (const ViewAngle& va : kAngles) {
            app.view_.phi   = va.phi;
            app.view_.theta = va.theta;
            app.view_.R     = kBaseR;
            shoot(std::string(phase) + "_" + va.name);
        }
    }

    app.Finalize();
    return 0;
}

// SMAA A/B 拍摄：固定光照/机位，只切 SMAA 开关，出两张图供肉眼对比。
int RunCaptureSmaa(const std::string& out_dir, const std::string& glb_path,
                   float threshold) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被 SMAA A/B（headless）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallSceneWithGltf(glb_path);

    // 正午侧光：叶片 cutout 边缘 / 细枝是观察锯齿最有代表性的目标。
    app.deg_ = jpov_skylight::SkyDegrees{};
    app.deg_.sun_elev_deg = 40.0f;
    app.deg_.sun_azim_deg = 60.0f;

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    app.SetSmaa(false);
    app.RunOnce(input, winfo, (out_dir + "/smaa_off.png").c_str());
    app.SetSmaa(true, threshold);
    app.RunOnce(input, winfo, (out_dir + "/smaa_on.png").c_str());
    LOG(INFO) << "SMAA A/B captured to " << out_dir
              << " (threshold=" << threshold << ")";
    app.Finalize();
    return 0;
}

// UI 截图（供验证面板布局）：面板可见，单帧出图。
int RunCaptureUi(const std::string& out_dir, const std::string& glb_path) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被查看器（面板截图）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(true);   // 画面板
    app.Init();
    app.InstallTextMeasure();
    app.InstallSceneWithGltf(glb_path);
    app.deg_.sun_elev_deg = 40.0f;
    app.deg_.sun_azim_deg = 60.0f;
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};
    const std::string path = out_dir + "/ui.png";
    app.RunOnce(input, winfo, path.c_str());
    LOG(INFO) << "capture(ui): " << path;
    app.Finalize();
    return 0;
}

// 风动拍摄：固定光照/机位，钉住风时间逐帧出图（人看动、AI 看数值），并附 wind_off 对照。
// 资产需带风数据（关节表 + loc6）；用 --glb plants/willow_wind_tex.glb。
int RunCaptureWind(const std::string& out_dir, const std::string& glb_path) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被风动（headless）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallSceneWithGltf(glb_path);

    app.deg_ = jpov_skylight::SkyDegrees{};
    app.deg_.sun_elev_deg = 45.0f;
    app.deg_.sun_azim_deg = 60.0f;
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    auto shoot = [&](const std::string& name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture(wind): " << path;
    };

    // 关风对照。
    app.SetWindEnabled(false);
    shoot("wind_off");

    // 开风：同一帧机位，钉住不同风时间（看摆动幅度/相位随时间变）。
    app.SetWindEnabled(true);
    app.SetWindFixedTime(0.0f);
    for (int k = 0; k < 5; ++k) {
        app.SetWindFixedTime(static_cast<float>(k) * 0.25f);
        char nm[32];
        std::snprintf(nm, sizeof(nm), "wind_t%03d", k * 25);
        shoot(nm);
    }

    app.Finalize();
    return 0;
}

// 吹倒（blow away）A/B 拍摄：固定光照/机位/风相位，只扫「逐实例吹倒比例」。
//   资产需带风数据（关节表 + loc6）；用 --glb plants/willow_wind_tex.glb。
int RunCaptureBlow(const std::string& out_dir, const std::string& glb_path) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被吹倒比例（headless）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallSceneWithGltf(glb_path);

    app.deg_ = jpov_skylight::SkyDegrees{};
    app.deg_.sun_elev_deg = 45.0f;
    app.deg_.sun_azim_deg = 60.0f;
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    // 钉住风时间 ⇒ 三张图只差吹倒比例（pivot 摇摆完全相同）。
    app.SetWindEnabled(true);
    app.SetWindFixedTime(0.35f);
    const float kRatios[] = {0.0f, 0.1f, 0.25f, 0.5f};
    for (float r : kRatios) {
        app.SetBlowAwayRatio(r);
        char nm[64];
        std::snprintf(nm, sizeof(nm), "blow_%03d", static_cast<int>(r * 100.0f + 0.5f));
        const std::string path = out_dir + "/" + nm + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture(blow): " << path;
    }

    // 特写：单株 + 拉近，检查枝条有没有被拉成三角形薄片（接缝 bug 的复查口径）。
    app.SetBlowAwayExponent(2.0f);
    app.SetInstanceCount(1);
    {
        const float keep_r = app.view_.R;
        app.view_.R = keep_r * 0.55f;
        app.SetBlowAwayRatio(0.0f);
        app.SetWindEnabled(false);
        app.RunOnce(input, winfo, (out_dir + "/detail_windoff.png").c_str());
        app.SetWindEnabled(true);
        app.RunOnce(input, winfo, (out_dir + "/detail_r000.png").c_str());
        app.SetBlowAwayRatio(0.5f);
        app.RunOnce(input, winfo, (out_dir + "/detail_r050.png").c_str());
        app.view_.R = keep_r;
    }

    // 实例数 A/B：同比例（0.25）下 1 / 4 / 9 株（验证滑条与网格排布）。
    app.SetBlowAwayRatio(0.25f);
    app.SetBlowAwayExponent(2.0f);
    for (int c : {1, 4, 9}) {
        app.SetInstanceCount(c);
        char nm[64];
        std::snprintf(nm, sizeof(nm), "count_%02d", c);
        const std::string path = out_dir + "/" + nm + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture(blow count): " << path;
    }
    app.SetInstanceCount(4);

    // 指数 A/B：同比例（0.5）下 n = 1（线性）/ 2（二次）/ 3。
    app.SetBlowAwayRatio(0.5f);
    for (float n : {1.0f, 2.0f, 3.0f}) {
        app.SetBlowAwayExponent(n);
        char nm[64];
        std::snprintf(nm, sizeof(nm), "blow_expo_%03d", static_cast<int>(n * 100.0f + 0.5f));
        const std::string path = out_dir + "/" + nm + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture(blow expo): " << path;
    }

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string glb_arg;          // --glb（空 = 用默认植物）
    std::string out_dir;          // --capture
    std::string smaa_dir;         // --capture-smaa
    std::string ui_dir;           // --capture-ui
    std::string wind_dir;         // --capture-wind
    std::string blow_dir;         // --capture-blow
    std::string save_mesh_prefix; // --save-mesh（导出当前风形态的网格）
    float save_blow = 0.5f;       // --blow
    float save_exponent = 2.0f;   // --blow-exponent
    float save_time = 0.35f;      // --wind-time
    float smaa_threshold = 0.1f;  // --smaa-threshold
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--glb") == 0 && i + 1 < argc) {
            glb_arg = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-smaa") == 0 && i + 1 < argc) {
            smaa_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-ui") == 0 && i + 1 < argc) {
            ui_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-wind") == 0 && i + 1 < argc) {
            wind_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-blow") == 0 && i + 1 < argc) {
            blow_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--save-mesh") == 0 && i + 1 < argc) {
            save_mesh_prefix = argv[i + 1];
        } else if (std::strcmp(argv[i], "--blow") == 0 && i + 1 < argc) {
            save_blow = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--blow-exponent") == 0 && i + 1 < argc) {
            save_exponent = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--wind-time") == 0 && i + 1 < argc) {
            save_time = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }

    const std::string glb_path = ResolveGlbArg(glb_arg);
    LOG(INFO) << "vegetation: 装载植物 GLB = " << glb_path;

    if (!save_mesh_prefix.empty()) {
        // 导出当前风形态的网格（CPU 端同公式），供离线核对 joint / 三角形面积。
        jpov::WindDeformParams p;
        p.time = save_time;
        p.amplitude = 0.35f;
        p.frequency = 0.5f;
        constexpr float kAz = 35.0f * 3.14159265358979323846f / 180.0f;
        p.direction = {std::cos(kAz), 0.0f, std::sin(kAz)};
        p.turb_amplitude = 0.12f;
        p.turb_frequency = 2.5f;
        p.gust_frequency = 0.6f;
        p.gust_strength = 0.7f;
        p.blow_ratio = save_blow;
        p.blow_exponent = save_exponent;
        return jpov_vegetation::SaveDeformedMesh(glb_path, save_mesh_prefix, p) ? 0 : 1;
    }

    if (!smaa_dir.empty()) {
        return RunCaptureSmaa(smaa_dir, glb_path, smaa_threshold);
    }
    if (!ui_dir.empty()) {
        return RunCaptureUi(ui_dir, glb_path);
    }
    if (!wind_dir.empty()) {
        return RunCaptureWind(wind_dir, glb_path);
    }
    if (!blow_dir.empty()) {
        return RunCaptureBlow(blow_dir, glb_path);
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, glb_path);
    }

    // ── 交互模式 ──
    JPOV::Config cfg;
    cfg.title      = "JPOV — 植被查看器（vegetation viewer）";
    cfg.width      = kWidth;
    cfg.height     = kHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_vegetation::kViewerFps);
    cfg.headless   = false;
    cfg.fonts      = ViewerFonts();

    VegetationViewerApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    app.InstallSceneWithGltf(glb_path);

    // 默认观察角：3/4 侧、略俯视，相机由 FitViewToPlant 自适应好距离。
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;

    app.Run();
    app.Finalize();
    return 0;
}

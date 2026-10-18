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

}  // namespace

int main(int argc, char** argv) {
    std::string glb_arg;          // --glb（空 = 用默认植物）
    std::string out_dir;          // --capture
    std::string smaa_dir;         // --capture-smaa
    std::string ui_dir;           // --capture-ui
    std::string wind_dir;         // --capture-wind
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
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }

    const std::string glb_path = ResolveGlbArg(glb_arg);
    LOG(INFO) << "vegetation: 装载植物 GLB = " << glb_path;

    if (!smaa_dir.empty()) {
        return RunCaptureSmaa(smaa_dir, glb_path, smaa_threshold);
    }
    if (!ui_dir.empty()) {
        return RunCaptureUi(ui_dir, glb_path);
    }
    if (!wind_dir.empty()) {
        return RunCaptureWind(wind_dir, glb_path);
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

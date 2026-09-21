// JPOV 天光查看器（skylight viewer）— 主程序
//
// 用途（Danis 需求）：一个专门肉眼验收**标准天光（SkyCommand）**的最小交互工具。
// 场景固定为三个不同 PBR 材质的方块（低反 / 高光 / 金属）+ 灰色地面，
// 视角变换与 model viewer 相同。
//
// 交互面板只有**四个自由度**（其余参数全走 SkyCommand 默认构造值）：
//   ① 浊度 turb [0,8]（默认 2）
//   ② 季节色温 [−1,+1]（左=蓝偏 / 右=红偏 / 中=中性）
//   ③ 太阳方向（仰角 [0,90] + 方位角 [0,360)）
//   ④ 月亮方向（仰角 [0,90] + 方位角 [0,360)）
// 天光由 `CreateDefaultSkyCommand` 构造，**光照（日光 + ambient）由它推导**；
// 月光平行光不做（月盘只作为天体画出）。
//
// headless 拍摄（--capture <out_dir>）：无窗口批量出图（供交付验收/自动化核对）。
//
// 架构：与 model viewer 同构（主程序只做装配 + 事件循环；场景/天光装配在
// skylight_scene.h，渲染核心 + 面板在 skylight_viewer_app.h）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_skylight_viewer
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_skylight_viewer.sh
//   → output/jpov_skylight_viewer/jpov_skylight_viewer

#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/demo/view_config.h"

namespace {

// 装配场景静态资源（交互与 headless 共用，避免两处各写一套而分叉）。
void InstallScene(jpov_skylight::SkylightApp& app) {
    app.box_mesh_    = app.RegisterMesh(jpov::MeshData::MakeBox(
        jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf,
        jpov_skylight::kBoxHalf));
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.mat_low_    = jpov_skylight::MaterialLowGloss();
    app.mat_high_   = jpov_skylight::MaterialHighGloss();
    app.mat_metal_  = jpov_skylight::MaterialMetal();
    app.mat_ground_ = jpov_skylight::GroundMaterial();
}

// headless 拍摄：按硬编码的一组场景出图，写到 out_dir。
// 这是交付验收用的确定性通路——不入 UI、不依赖交互。
int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 天光查看器（headless capture）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);    // 纯 3D 截图（无面板）
    app.Init();
    InstallScene(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 把相机的**方位**对准某个天体（用于单独看月盘）：
    //   ViewConfig pos=(R·cosφ·sinθ, R·sinφ, R·cosφ·cosθ)，视线 = −normalize(pos)，
    //   target 固定原点。要视线方位 = dir(elev,azim) 的方位角，取 θ = azim + 180°。
    //
    // ⚠️ 两处约束（都是踩过的坑）：
    //   1. 不能同时把**仰角**也对准：视线仰角 = −φ，φ<0 会把相机压到地面以下
    //      （y = R·sinφ < 0），40×40 的地面会挡住天空。故只对**方位**，仰角靠
    //      取能落进视野的天体仰角（FOV 60° → 可见仰角约 ±30°）。
    //   2. φ 不能取严格 0：相机恰与 target 同高时为**正水平**视线，画面上会留下
    //      一条**逆 VP 奇异带**（wp.w→0，dir 归零/未归一），那条带上的天体会画不出来。
    //      取一个小正 φ（相机略高于原点）即可避开。
    auto aim_at_azimuth = [&](float azim_deg) {
        const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        app.view_.phi   = 0.05;  // 略高于原点（避免 y=0 正水平时的逆 VP 奇异带）
        app.view_.theta = static_cast<double>(azim_deg + 180.0f) * kDeg2Rad;
    };

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 相机：三方块斜前方（theta=45° 在 +X/+Z 象限；R=6 使三块完整入画）。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.0;

    // ── A. 昼夜扫谱（太阳仰角递减，固定其余自由度）──
    // 场景 + 月色，验证天色/日照/环境光在同一条时间轴上平滑过渡。
    app.moon_elev_deg_ = 30.0f;
    app.moon_azim_deg_ = 225.0f;
    app.turbidity_     = jpov_skylight::kTurbidityDef;
    app.season_tint_   = 0.0f;
    app.sun_azim_deg_  = 45.0f;
    for (int e : {90, 30, 10, 4, 0}) {
        app.sun_elev_deg_ = static_cast<float>(e);
        shoot(("sun_elev" + std::to_string(e)).c_str());
    }

    // ── B. 浊度扫谱（固定默认正午 + 月盘，看霾化与月晕变宽）──
    app.sun_elev_deg_ = 45.0f;
    app.moon_elev_deg_ = 30.0f;
    for (int t : {0, 2, 5, 8}) {
        app.turbidity_ = static_cast<float>(t);
        shoot(("turb" + std::to_string(t)).c_str());
    }
    app.turbidity_ = jpov_skylight::kTurbidityDef;

    // ── C. 季节色温扫谱（蓝偏 .. 红偏，看冷暖偏色）──
    for (int s : {-1, 0, 1}) {
        app.season_tint_ = static_cast<float>(s);
        shoot(("season" + std::to_string(s + 1)).c_str());
    }
    app.season_tint_ = 0.0f;

    // ── D. 月亮方位/仰角：月盘在天球不同位置 ──
    // 相机对准月亮方位（aim_at：φ=−仰角、θ=方位角+180°），太阳落山（0°）→
    // 只剩夜色 + 月盘，便于单独看月盘位置与月晕。
    app.sun_elev_deg_ = 0.0f;
    app.moon_azim_deg_ = 90.0f;   // 月亮在 +X 侧（方位角 90°=+X）
    aim_at_azimuth(app.moon_azim_deg_);
    for (int e : {25, 10, 3}) {
        app.moon_elev_deg_ = static_cast<float>(e);
        shoot(("moon_elev" + std::to_string(e)).c_str());
    }
    // 月亮降到 0°（贴地平线）→ 被俯仰角重映射压进地平线，整盘淹没
    app.moon_elev_deg_ = 0.0f;
    shoot("moon_at_horizon");

    // ── E. 月亮藏匿验证：月亮真仰角 < 0（沉到地平线下）→ 月盘完全不可见 ──
    // 仰角滑条下限是 0°，但接口不限制（moon_dir.y<0 即沉下）；直接给负仰角验证。
    app.moon_elev_deg_ = -20.0f;
    shoot("moon_below_horizon");
    app.moon_elev_deg_ = 30.0f;

    // 恢复默认视角（后续场景拍摄用）
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.0;

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
    // 用法：jpov_skylight_viewer --capture /tmp/sky_out
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            return RunCapture(argv[i + 1]);
        }
    }

    // ── 交互模式 ──
    JPOV::Config cfg;
    cfg.title  = "JPOV — 天光查看器（skylight viewer）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_skylight::kViewerFps);
    cfg.headless   = false;
    // 显式声明字体：CJK 显中文滑条标签，Latin 做拉丁回退（路径相对 exe 的 fonts/，
    // build_jpov_skylight_viewer.sh 同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();

    // ── 场景静态资源（只建一次，不在 OneIteration 里重复构造/上传）──
    InstallScene(app);

    // ── 初始视角：三方块斜前方，R=6 使三块（总宽 ~4.2m）完整入画。──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;                       // 略俯视（~14°）
    app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
    app.view_.R     = 6.0;

    // 交互事件循环（阻塞）。
    app.Run();

    app.Finalize();
    return 0;
}

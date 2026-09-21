// JPOV 天光查看器（skylight viewer）— 主程序
//
// 用途（Danis 需求）：一个专门肉眼验收**天光（SkyCommand）**的最小交互工具。
// 场景固定为三个不同 PBR 材质的方块（低反 / 高光 / 金属）+ 灰色地面，
// 视角变换与 model viewer 相同；滑条可调太阳仰角（0~90°）、浊度、季节、太阳强度、
// 环境光强度、夜色强度、月亮仰角、月盘亮度、月晕强度。
//
// 验收路径（夜色）：把「太阳仰角 °」滑到 0 → 白天项被 shader 精确压到 0，画面只剩
// 夜色（night_zenith_color → night_horizon_color 的 van Rhijn 垂直渐变）；再把
// 「夜色强度」拉到 0 做对照。
// 验收路径（月盘）：把「月盘亮度」拉大（默认 100，已可见）→ 月盘出现在 −X 侧；
// 调「月亮仰角 °」到 0 → 月盘被俯仰角重映射压进地平线（整盘淹没，不露半拉盘）；
// 调「月晕强度」> 0，再拉「浊度 turb」→ **月晕随浊度变宽**（本 PR 重点）。
//
// headless 拍摄（--capture）：除交互模式外，支持无窗口批量出图（供交付验收/
// 自动化核对）；用法见 main 里 --capture 分支的注释。
//
// 架构：与 model viewer 同构（主程序只做装配 + 事件循环；场景/光照在
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

// headless 拍摄：按硬编码的一组场景出图（纯 3D，无面板），写到 out_dir。
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
    app.SetShowScene(false);    // 关掉三方块/地面——本组拍的是**天空本身**
    app.Init();
    InstallScene(app);

    // 相机：对准月盘方位（月盘在 −X 侧、仰角由滑条给）。
    // ViewConfig 公式：pos=(R·cosφ·sinθ, R·sinφ, R·cosφ·cosθ)，视线 = −normalize(pos)。
    // θ=90° → 相机在 +X 轴看向原点（视线沿 −X），即**正对** −X 侧的月盘方位；
    // φ=−0.25 → 相机略低于原点、视线略带仰角（~14°），使月盘落在画面中上部。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = -0.25;
    app.view_.theta = 3.14159265358979323846 / 2.0;   // 90°：视线沿 −X
    app.view_.R     = 6.0;

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 夜景基准：太阳落山（0°），留纯夜色 + 月盘。
    app.elev_deg_ = 0.0f;
    app.sun_intensity_ = 0.0f;        // 关直射（只看天光/月盘）
    app.ambient_intensity_ = 0.0f;
    app.night_scale_ = 1.0f;
    app.moon_elev_deg_ = 20.0f;
    app.moon_brightness_ = 5.0f;
    app.moon_glow_ = 0.0f;
    app.turbidity_ = 2.0f;
    shoot("moon_night_base");        // 月盘（无晕）夜景基准

    // 月盘俯仰角重映射：月亮真实仰角 0° → 整盘淹没（不露半拉盘）。
    app.moon_elev_deg_ = 0.0f;
    shoot("moon_elev0");             // 0° 时整盘淹没
    app.moon_elev_deg_ = 20.0f;
    shoot("moon_elev20");            // 20° 月盘在真实位置

    // ⭐ 本 PR 重点：月晕宽度随浊度变化（固定月盘/亮度/晕强，只改浊度）。
    app.moon_glow_ = 0.1f;
    app.turbidity_ = 2.0f;
    shoot("moon_glow_turb2");        // 清澈：晕窄
    app.turbidity_ = 8.0f;
    shoot("moon_glow_turb8");        // 重霾：晕宽一倍

    // 对照：完全关闭月盘（亮度 0）→ 只剩夜色。
    app.turbidity_ = 2.0f;
    app.moon_glow_ = 0.0f;
    app.moon_brightness_ = 0.0f;
    shoot("moon_off_control");       // 无月盘对照

    // 带场景的对照：开三方块 + 给一点环境光，看月盘与物体的同屏效果。
    // （默认环境光 0.3 的“物体光照不随夜色变”已知边界见设计笔记 5.2）
    app.SetShowScene(true);
    app.ambient_intensity_ = 0.3f;
    app.moon_brightness_ = 100.0f;
    app.moon_glow_ = 3.0f;
    app.view_.phi   = 0.25;
    app.view_.theta = 3.0f * 3.14159265358979323846 / 4.0;  // 斜看三方块
    shoot("moon_with_scene");        // 三方块 + 月盘同屏

    // ── 昼夜过渡扫谱（环境光色/强连续性验收）：场景 + 夜色 + 月盘，扫太阳仰角 ──
    app.SetShowScene(true);
    app.ambient_intensity_ = 1.0f;   // 走 AmbientIntensity 曲线（含夜色项）
    app.moon_brightness_ = 5.0f;
    app.moon_glow_ = 0.1f;
    app.view_.phi   = 0.15;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    for (int e : {20, 8, 4, 2, 0}) {
        app.elev_deg_ = static_cast<float>(e);
        shoot(("transition_elev" + std::to_string(e)).c_str());
    }

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

    // ── 初始视角：三方块斜前方（theta=45° 的相机在 +X/+Z 象限，正好看到 +X 侧的
    // 日落方向与方块高光），R 取 6 米使三块（总宽 ~4.2m）完整入画。──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;                       // 略俯视（~14°）
    app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
    app.view_.R     = 6.0;

    // 交互事件循环（阻塞）。
    app.Run();

    app.Finalize();
    return 0;
}

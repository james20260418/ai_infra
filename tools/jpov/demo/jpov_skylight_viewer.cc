// JPOV 天光查看器（skylight viewer）— 主程序
//
// 用途：一个专门肉眼验收**标准天光（SkyCommand）**的最小交互工具。
// 场景固定为三个不同 PBR 材质的方块（低反 / 高光 / 金属）+ 灰色地面，
// 视角变换与 model viewer 相同。
//
// 交互面板有五个自由度（其余参数全走 SkyCommand 默认构造值）：
//   ① 浊度 turb [0,8]（默认 2）
//   ② 季节色温 日光 [−1,+1]（左=蓝偏 / 右=红偏 / 中=中性；只染太阳能通道）
//   ③ 天体方向（仰角 [−90,+90] + 方位角 [0,360)）
//   ④ 月色变红 [0,1]（0=常月，1=血月；只染月盘 + 月光）
//   ⑤ 夜空偏蓝 [0,1]（0=出厂夜色，1=梦幻蓝且更亮）
// 天光由 `CreateDefaultSkyCommand` 构造，**主平行光 + ambient 由它推导**。
// 月亮恒在反日点（moon_dir = −sun_dir）：仰角为正时主光是太阳、为负时主光是月亮
//（两者在地平线 0° 处交接），一条仰角轴即可验收日/月两种主光的强度与颜色。
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

#include <cstdio>
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

// 额外模型（桌子 / 高模橡树）的**相对路径**（相对 exe 所在目录）。
// build_jpov_skylight_viewer.sh 把两个 glb 拷到 output/<demo>/models/ 下；
// 分发态即“exe 旁 models/” —— 与字体同一套“拷贝法”惯例（SOUL：路径硬编码 + 脚本拷贝）。
inline constexpr const char* kTableModelPath = "models/table.glb";
inline constexpr const char* kOakModelPath   = "models/tripo_oak_4k.glb";

// 装载额外模型并摆到三方块周囲（供标定夜色 ambient 时观察物体受光）。
// 位置/朝向由肉眼调：桌子放左侧（−X）、橡树放右侧（+X），都落在 40×40 地面内。
//
// 缩放：资产原始尺寸差异很大（橡树是 Tripo 生成的高模，原始尺寸未知），故按
// **目标世界高度** 自动归一：scale = 目标高 / 资产包围盒高（Y 向）。若 bounds 无效
// 则 scale=1（不静默乱缩，宁可看得出来不对）。
void InstallModels(jpov_skylight::SkylightApp& app) {
    auto load = [&app](const char* rel) {
        jpov::GltfObject o = app.LoadGltf(rel);
        CHECK(!o.empty()) << "LoadGltf failed: " << rel
                          << "（分发态需 build 脚本把模型拷到 exe 旁 models/）";
        return o;
    };
    // 按目标高度求缩放因子（bounds 无效时返回 1.0，不静默乱缩）。
    auto scale_to_height = [](const jpov::GltfObject& o, float target_h) {
        if (!o.bounds_valid) {
            LOG(WARNING) << "模型无 bounds，scale 置 1（不自动归一）";
            return 1.0f;
        }
        const float h = o.bounds_max[1] - o.bounds_min[1];
        if (h <= 1.0e-6f) {
            LOG(WARNING) << "模型 Y 向高度≈0，scale 置 1（不自动归一）";
            return 1.0f;
        }
        return target_h / h;
    };

    // 桌子（低矮家具）：放到左侧，目标高 ~0.75 m（现代餐桌面高）。
    jpov::GltfObject table = load(kTableModelPath);
    const float table_scale = scale_to_height(table, 0.75f);
    app.AddModel(std::move(table), /*center*/ {-2.6f, 0.0f, -0.4f},
                 /*up*/ {0, 1, 0}, /*front*/ {1, 0, 0}, table_scale);

    // 高模橡树：放到右侧，目标高 ~6 m（成熟橡树）。
    jpov::GltfObject oak = load(kOakModelPath);
    const float oak_scale = scale_to_height(oak, 6.0f);
    app.AddModel(std::move(oak), /*center*/ {3.2f, 0.0f, -0.6f},
                 /*up*/ {0, 1, 0}, /*front*/ {-1, 0, 0}, oak_scale);
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
    InstallModels(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 把相机的**方位**对准某个天体（用于单独看月盘）：
    //   ViewConfig pos=(R·cosφ·sinθ, R·sinφ, R·cosφ·cosθ)，视线 = −normalize(pos)，
    //   target 固定原点。视线方位 = dir(elev,azim) 的方位角 ⇒ θ = azim + 180°。
    //
    //   只对**方位**、不追**仰角**：视线仰角 = −φ，φ<0 会把相机压到地面以下
    //   （y = R·sinφ < 0），40×40 地面挡住天空；天体落进视野靠 FOV 60°
    //   （可见仰角约 ±30°）。
    //   φ 取小正值（相机略高于原点）：正水平视线（φ=0）会落在逆 VP 的奇异带
    //   （wp.w→0，dir 未归一），该带上的天体画不出来。
    auto aim_at_azimuth = [&](float azim_deg) {
        const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        app.view_.phi   = 0.05;  // 相机略高于原点（避开 φ=0 正水平视线的逆 VP 奇异带）
        app.view_.theta = static_cast<double>(azim_deg + 180.0f) * kDeg2Rad;
    };

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 相机：三方块 + 桌子/橡树整体入画（拉远 + 抬高，整体宽 ~9m、橡树高 6m）。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.35;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 9.5;

    // ── A. 昼夜扫谱（天体仰角 +90° → −90°，固定其余自由度）──
    // 同一条仰角轴从正午扫到午夜：>0 段主光是太阳、<0 段主光是月亮（恒在 −sun_dir）。
    // 验证天色 / 主光（日↔月）/ 环境光在同一条时间轴上平滑过渡，且在 0° 处交接
    //（太阳与月亮都在地平线，两侧主光强度都趋零）。
    app.deg_.turbidity    = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season  = 0.0f;
    app.deg_.sun_azim_deg = 45.0f;
    for (int e : {90, 45, 30, 10, 4, 0, -4, -10, -30, -45, -60, -90}) {
        app.deg_.sun_elev_deg = static_cast<float>(e);
        shoot(("body_elev" + std::to_string(e)).c_str());
    }

    // ── B. 浊度扫谱（夜间月光 + 月盘，看霾化与月晕变宽 + 月光 turb 衰减）──
    // 仰角取 −30°（夜间，月亮升到 +30°）：一图同看天色霾化、月盘/月晕变宽、
    // 以及月光主光被 TurbSunLoss 衰减。
    app.deg_.sun_elev_deg = -30.0f;
    for (int t : {0, 2, 5, 8}) {
        app.deg_.turbidity = static_cast<float>(t);
        shoot(("turb" + std::to_string(t)).c_str());
    }
    app.deg_.turbidity = jpov_skylight::kTurbidityDef;

    // ── C. 日光季节色温扫谱（蓝偏 .. 红偏，看冷暖偏色；白天日光）──
    app.deg_.sun_elev_deg = 45.0f;
    for (int s : {-1, 0, 1}) {
        app.deg_.daylight_season = static_cast<float>(s);
        shoot(("season" + std::to_string(s + 1)).c_str());
    }
    app.deg_.daylight_season = 0.0f;

    // ── C2. 日光季节色温扫谱（夜间，月亮在 +30°）：验收它**只染太阳能通道**──
    // 预期：天色（夜色）与月盘**不变**，只有环境光的“暮色端”随白光程度变。
    app.deg_.sun_elev_deg = -30.0f;
    for (int s : {-1, 0, 1}) {
        app.deg_.daylight_season = static_cast<float>(s);
        shoot(("season_night" + std::to_string(s + 1)).c_str());
    }
    app.deg_.daylight_season = 0.0f;

    // ── C3. 谷色变红扫谱（夜景 + 全景，月亮在 +30°）——月盘与月光一起变红变暗 ──
    app.deg_.sun_elev_deg = -30.0f;
    for (float mr : {0.0f, 0.5f, 1.0f}) {
        app.deg_.moon_red = mr;
        shoot(("moon_red" + std::to_string(static_cast<int>(mr * 10))).c_str());
    }
    app.deg_.moon_red = 0.0f;

    // ── C4. 夜蓝扫谱（夜景，月亮在 +30°）——夜色两色整体偏蓝且变亮 ──
    for (float nb : {0.0f, 0.5f, 1.0f}) {
        app.deg_.night_blue = nb;
        shoot(("night_blue" + std::to_string(static_cast<int>(nb * 10))).c_str());
    }
    app.deg_.night_blue = 0.0f;

    // ── D. 月亮位置扫谱（月亮仰角 = −天体仰角；相机对准月亮方位）──
    // 月亮方位 = sun_azim + 180 = 90°（+X 侧）⇒ 取 sun_azim = 270°。
    // 天体仰角取负值 ⇒ 月亮升到 25°/10°/3°；相机对准月亮方位看月盘与月晕。
    app.deg_.sun_azim_deg = 270.0f;
    aim_at_azimuth(90.0f);

    // 对准月亮时扫“谷色变红” → 直接看月盘颜色（夜间）。
    // 日光季节色温**不该**改变月盘（daylight_season 不吃月亮），此处一并验证。
    app.deg_.sun_elev_deg = -25.0f;
    for (float mr : {0.0f, 0.5f, 1.0f}) {
        app.deg_.moon_red = mr;
        shoot(("moon_season" + std::to_string(static_cast<int>(mr * 10))).c_str());
    }
    app.deg_.moon_red = 0.0f;

    for (int e : {-25, -10, -3}) {
        app.deg_.sun_elev_deg = static_cast<float>(e);
        shoot(("moon_elev" + std::to_string(-e)).c_str());
    }
    // 月亮贴地平线（天体仰角 0°）→ 月盘被俯仰角重映射压进地平线，整盘淹没
    app.deg_.sun_elev_deg = 0.0f;
    shoot("moon_at_horizon");

    // ── E. 月亮藏匿验证：月亮真仰角 < 0（沉到地平线下）→ 月盘完全不可见 ──
    // 天体仰角 +20° ⇒ 月亮在 −20°（地平线下）；接口不限制负的 moon_dir.y。
    app.deg_.sun_elev_deg = 20.0f;
    shoot("moon_below_horizon");

    // 恢复默认视角（后续场景拍摄用）
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.0;

    // ── F. 模型放置自检：正午（看桌子/橡树是否正常入画）──
    app.deg_.sun_elev_deg = 60.0f;
    app.deg_.sun_azim_deg = 45.0f;
    shoot("scene_noon_check");

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
    // 用法：jpov_skylight_viewer --capture /tmp/sky_out
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir);
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
    InstallModels(app);

    // ── 初始视角：三方块斜前方，R=6 使三块（总宽 ~4.2m）完整入画。──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;                       // 略俯视（~14°）
    app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
    app.view_.R     = 9.5;

    // 交互事件循环（阻塞）。
    app.Run();

    app.Finalize();
    return 0;
}

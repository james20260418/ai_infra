// JPOV 雾火查看器（fire_fog viewer）— 主程序
//
// 用途：一个专门肉眼验收**froxel 体积雾**（设计见 docs/jpov_froxel_design.md）的最小交互工具。
// 场景 = 灰色地面 + 一棵高模橡树（oak）；点状雾默认摆在橡树中段，便于观察雾与实体的
// 深度关系（雾在树前正常叠加，树前的雾被场景深度裁剪，树后的雾被树遮挡）。
//
// 交互面板（见 fire_fog_viewer_app.h）：雾中心 x/y/z、半径、消光 σ、衰减剖面、散射色 albedo、
// 自发光 emission，以及一盏点光源、太阳仰角/方位角/浊度，与 god ray 相位/增益。
// ⚠️ 雾火始终走物理光照（ambient + 太阳×CSM + 点光源）；「无光照」链路已移除。
//
// headless 拍摄（--capture <out_dir>）：无窗口批量出图（供交付验收/自动化核对）。
//
// 架构：主程序只做装配 + 事件循环；场景/渲染核心 + 面板在 fire_fog_viewer_app.h。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_fire_fog_viewer
//   或 sh 脚本：./tools/jpov/build_jpov_fire_fog_viewer.sh
//   → output/jpov_fire_fog_viewer/jpov_fire_fog_viewer

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/fire_fog_viewer_app.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/common/utils.h"

namespace {

// 橡树模型（相对路径，相对 exe 所在目录）；build 脚本把 glb 拷到 exe 旁 models/。
inline constexpr const char* kOakModelPath = "models/tripo_oak_4k.glb";
inline constexpr float kOakTargetHeight = 6.0f;   // 归一后的世界高度（米）

// 装配场景静态资源（交互与 headless 共用，避免两处各写一套而分叉）。
void InstallScene(jpov_fire_fog::FireFogApp& app) {
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.ground_mat_  = jpov_skylight::GroundMaterial();

    jpov::GltfObject oak = app.LoadGltf(kOakModelPath);
    CHECK(!oak.empty()) << "LoadGltf failed: " << kOakModelPath
                        << "（分发态需 build 脚本把模型拷到 exe 旁 models/）";
    // 按目标高度求缩放因子（bounds 无效时返回 1.0，不静默乱缩）。
    float scale = 1.0f;
    if (oak.bounds_valid) {
        const float hgt = oak.bounds_max[1] - oak.bounds_min[1];
        if (hgt > 1.0e-6f) {
            scale = kOakTargetHeight / hgt;
        } else {
            LOG(WARNING) << "橡树模型 Y 向高度≈0，scale 置 1（不自动归一）";
        }
    } else {
        LOG(WARNING) << "橡树模型无 bounds，scale 置 1（不自动归一）";
    }
    app.oak_ = std::move(oak);
    app.oak_center_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    app.oak_up_ = jpov::Vec3f(0.0f, 1.0f, 0.0f);
    app.oak_front_ = jpov::Vec3f(0.0f, 0.0f, 1.0f);
    app.oak_scale_ = scale;
}

// 默认视角：橡树（高 6m）斜前方，看全树 + 雾。
void DefaultView(jpov_fire_fog::FireFogApp& app) {
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;                          // 略俯视（~14°）
    app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
    app.view_.R     = 14.0;
}

// headless 拍摄：一组确定性视图/参数，写到 out_dir。
int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title  = "JPOV — 雾火查看器（headless capture）";
    cfg.width  = jpov_fire_fog::kViewerWidth;
    cfg.height = jpov_fire_fog::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_fire_fog::FireFogApp app(cfg);
    app.SetShowPanel(false);   // 纯 3D 截图（无面板）
    app.Init();
    InstallScene(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_fire_fog::kViewerWidth;
    winfo.height = jpov_fire_fog::kViewerHeight;
    jpov::InputSnapshot input{};

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        const auto t0 = std::chrono::steady_clock::now();
        app.RunOnce(input, winfo, path.c_str());
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        LOG(INFO) << "capture: " << path << "  (" << ms << " ms)";
    };

    DefaultView(app);
    app.deg_.sun_elev_deg = 18.0f;   // 低太阳 ⇒ 长影、god ray 更明显
    app.deg_.turbidity = 2.0f;

    // ── A. 无雾基线 / 默认雾（对照；雾始终走物理光照）──
    app.SetShowFog(false);
    shoot("01_baseline_no_fog");
    app.SetShowFog(true);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 3.0f;
    app.fog_sigma_ = 0.6f;
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);
    shoot("02_fog_default");

    // ── B. 衰减剖面扫谱（4 种）──
    for (int a = 0; a < 4; ++a) {
        app.fog_attenuation_ = a;
        shoot(("03_atten_" + std::to_string(a)).c_str());
    }
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);

    // ── C. 消光 σ 扫谱（1/m；看由透明到浓）──
    for (float s : {0.2f, 0.6f, 1.2f, 2.0f}) {
        app.fog_sigma_ = s;
        shoot(("04_sigma_" + std::to_string(static_cast<int>(s * 10))).c_str());
    }
    app.fog_sigma_ = 0.6f;

    // ── D. 半径扫谱（看球大小；雾贴着树 / 吞掉树）──
    for (float r : {1.0f, 3.0f, 5.0f}) {
        app.fog_radius_ = r;
        shoot(("05_radius_" + std::to_string(static_cast<int>(r))).c_str());
    }
    app.fog_radius_ = 3.0f;

    // ── E. 深度遮挡（3 例，验证场景深度裁剪是否生效）──
    app.fog_radius_ = 2.5f;
    app.fog_sigma_ = 0.8f;
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);
    auto head_on = [&]() {
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = 0.12;   // 几乎平视
        app.view_.theta = 0.0;    // 相机在 +Z 侧
        app.view_.R     = 13.0;
    };
    // E1：雾整个在地面之下 ⇒ 应被地面**完全遮挡**。
    head_on();
    app.fog_center_ = jpov::Vec3f(0.0f, -3.0f, 0.0f);
    shoot("06_occlusion_below_ground");
    // E2：雾在树**之后**（−Z 侧）⇒ 树应遮挡雾的中段。
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, -6.0f);
    shoot("07_occlusion_behind_tree");
    // E3：雾在树**之前**（+Z 侧）⇒ 雾叠盖在树上。
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 6.0f);
    shoot("08_in_front_of_tree");

    // ── F. 相机环绕（看立体感；雾固定树处）──
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 3.0f;
    app.fog_sigma_ = 0.6f;
    for (int deg : {0, 90, 180, 270}) {
        DefaultView(app);
        app.view_.theta = static_cast<double>(deg) * 3.14159265358979323846 / 180.0;
        shoot(("09_orbit_" + std::to_string(deg)).c_str());
    }

    // ── G. 大雾体（覆盖树与地面交界的深度范围）──
    DefaultView(app);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 8.0f;
    app.fog_sigma_ = 0.4f;
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);
    shoot("10_big_fog");

    // ── I. froxel 分辨率对比：Nz/tile 组合 ──
    DefaultView(app);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 3.0f;
    app.fog_sigma_ = 0.6f;
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);
    {
        // (Nz index, tile index): (256,16) / (64,8) / (64,16)
        const int cases[3][2] = {{1, 1}, {0, 0}, {0, 1}};
        const char* names[3] = {"30_nz256_tile16", "31_nz64_tile8", "32_nz64_tile16"};
        for (int c = 0; c < 3; ++c) {
            app.nz_index_ = cases[c][0];
            app.tile_px_index_ = cases[c][1];
            shoot(names[c]);
        }
    }
    app.nz_index_ = 1;
    app.tile_px_index_ = 1;

    // ── J. z 远端扫谱（range ↔ 分层带）──
    DefaultView(app);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 3.0f;
    app.fog_sigma_ = 0.6f;
    app.fog_attenuation_ = static_cast<int>(jpov::FogAttenuation::kQuadratic);
    for (float fz : {200.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f}) {
        app.z_far_ = fz;
        shoot(("40_zfar_" + std::to_string(static_cast<int>(fz))).c_str());
    }
    app.z_far_ = 2000.0f;

    // ── K. albedo（散射色）扫谱：中性灰 → 红 → 蓝 ──
    DefaultView(app);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 3.0f;
    app.fog_sigma_ = 0.8f;
    app.fog_emission_ = jpov::Color{0.0f, 0.0f, 0.0f, 1.0f};
    app.fog_albedo_ = jpov::Color{0.9f, 0.9f, 0.9f, 1.0f};
    shoot("45_albedo_gray");
    app.fog_albedo_ = jpov::Color{1.0f, 0.25f, 0.2f, 1.0f};
    shoot("46_albedo_red");
    app.fog_albedo_ = jpov::Color{0.2f, 0.35f, 1.0f, 1.0f};
    shoot("47_albedo_blue");
    app.fog_albedo_ = jpov::Color{0.9f, 0.9f, 0.9f, 1.0f};

    // ── L. emission（自发光）扫谱：0 → 弱 → 强（验证自发光与光照解耦）──
    //  先关太阳增益、降 ambient 使自发光主导（用 sun_gain=0 压太阳项）。
    app.sun_gain_ = 0.0f;
    for (float e : {0.0f, 0.3f, 1.0f, 3.0f}) {
        app.fog_emission_ = jpov::Color{e, e * 0.6f, e * 0.3f, 1.0f};
        shoot(("48_emission_" + std::to_string(static_cast<int>(e * 10))).c_str());
    }
    app.fog_emission_ = jpov::Color{0.0f, 0.0f, 0.0f, 1.0f};
    app.sun_gain_ = 2.0f;

    // ── M. 点光源（fire_fog 自持 Nxy tile culling）：关/开 + 位置扫谱 ──
    DefaultView(app);
    app.fog_center_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    app.fog_radius_ = 5.0f;
    app.fog_sigma_ = 0.8f;
    app.fog_albedo_ = jpov::Color{0.9f, 0.9f, 0.9f, 1.0f};
    app.deg_.sun_elev_deg = 8.0f;
    app.point_light_enable_ = false;
    shoot("50_pl_off");
    app.point_light_enable_ = true;
    app.point_light_pos_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);
    shoot("51_pl_center");
    app.point_light_pos_ = jpov::Vec3f(4.0f, 3.0f, 0.0f);
    shoot("52_pl_side");
    app.point_light_pos_ = jpov::Vec3f(0.0f, 6.0f, 0.0f);
    shoot("53_pl_top");
    app.point_light_color_ = jpov::Color{1.0f, 0.3f, 0.2f, 1.0f};
    shoot("54_pl_red");
    app.point_light_color_ = jpov::Color{1.0f, 0.85f, 0.6f, 1.0f};
    app.point_light_pos_ = jpov::Vec3f(0.0f, 3.0f, 0.0f);

    // ── N. god ray：太阳方位扫谱 + 增益对照（ambient + 太阳×CSM）──
    app.fog_center_ = jpov::Vec3f(0.0f, 4.0f, 0.0f);
    app.fog_radius_ = 10.0f;      // 大雾体：让树影确实切到雾
    app.fog_sigma_ = 0.5f;
    app.sun_phase_g_ = 0.7f;
    app.sun_gain_ = 2.0f;
    app.deg_.sun_elev_deg = 8.0f;
    for (int az : {0, 90, 180, 270}) {
        app.deg_.sun_azim_deg = static_cast<float>(az);
        DefaultView(app);
        shoot(("20_godray_az" + std::to_string(az)).c_str());
    }
    app.deg_.sun_azim_deg = 270.0f;
    DefaultView(app);
    app.sun_gain_ = 0.0f;
    shoot("21_sun_gain0");
    app.sun_gain_ = 2.0f;
    shoot("22_sun_gain2");

    // ── 诊断：带 UI 面板拍两张（验证 2D/UI 是否被雾火管线破坏）——无雾 / 有雾。──
    DefaultView(app);
    app.InstallTextMeasure();
    app.SetShowPanel(true);
    app.SetShowFog(false);
    shoot("98_panel_nofog");
    app.SetShowFog(true);
    shoot("99_panel_ui");

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
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
    cfg.title  = "JPOV — 雾火查看器（fire_fog viewer）";
    cfg.width  = jpov_fire_fog::kViewerWidth;
    cfg.height = jpov_fire_fog::kViewerHeight;
    cfg.resizable  = false;
    // froxel 管线较重（全分辨率 inject/scatter）⇒ 交互降到 30fps（Danis 2026-10-09）。
    cfg.target_fps = 30;
    cfg.headless   = false;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_fire_fog::FireFogApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    InstallScene(app);

    // 初始视角：橡树斜前方。
    DefaultView(app);
    app.deg_.sun_elev_deg = 18.0f;   // 低太阳 ⇒ 长影、god ray 更明显
    app.deg_.turbidity = 2.0f;

    app.Run();

    app.Finalize();
    return 0;
}

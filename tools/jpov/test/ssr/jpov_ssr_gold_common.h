// JPOV SSR gold test —— 共享场景 / 机位 / 开关（generator 与 test 共用）
//
// 目的：给「SSR 链路」上**成图**保护。SSR 之前只有 CPU↔GPU 对拍（几何与命中格）
// 与 tile 等价性（并发粒度），没有任何一张"整帧观感"的基线 —— 于是像"深度纹理换成
// w = 1/z"这种改动（数学等价、舍入不同）无法验收。这里用天光查看器的**镜面场景**
// 拍 11 张固定组合（5 个基础 + 6 个「能力各见效」），作为 SSR 输出的端到端 gold。
//
// 前 5 个组合刻意覆盖不同失效面（都是"改坏了才看得出来"的角度）：
//   1. graze_mirror     极掠射（离地 6 cm / 45 m）—— 深度精度最敏感，条纹/黑带的老病灶
//   2. mirror_1x        1:1 全分辨率 —— 反射最锐，取色/命中错一格立刻可见
//   3. metal_mid        金属 r=0.30 + 中等俯角 —— 磨砂 mip 链（LOD>0）
//   4. ladder_high      四段拼接 + 高俯角 —— 同屏四种材质（逐像素 F0/roughness）
//   5. dawn_metal_third 低太阳 + 1:3 降采样 —— 非 2 的幂的分辨率、长影子
//
// 后 6 个（2026-11-13）每个只打开一项「遮丑」能力，机位是为「让这一项真的起作用」挑的；
// 每个组合还带一条 min_ab_ratio：ON 与「六项全关」的差异必须 ≥ 它（否则说明这项没生效，
// 后面的 gold 比较就变成空测 —— 这条自检就是为了防"gold 很绿但什么都没测到"）。
//
// 场景/资产与交互查看器的「场景1」同源（镜面地板 + 24 个散布方块 + 桌子/橡树 +
// 4 个镜面蓝人），但**全部参数在这里钉死**：gold 不能依赖面板默认值。

#ifndef JPOV_TEST_SSR_JPOV_SSR_GOLD_COMMON_H_
#define JPOV_TEST_SSR_JPOV_SSR_GOLD_COMMON_H_

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace jpov_ssr_gold {

// 一帧的全部自变量。默认值 = 「场景1 + 默认天光」的常规机位。
struct SsrGoldCase {
    const char* name = "";
    float view_phi = 0.21f;          // 相机仰角（弧度）
    float view_theta = 0.0f;         // 相机方位（弧度）
    float view_R = 45.0f;            // 相机距离（米）
    float sun_elev_deg = 35.0f;
    float sun_azim_deg = 60.0f;
    float turbidity = jpov_skylight::kTurbidityDef;
    int floor_preset = 0;            // 0 镜面 / 1 金属 / 2 石砖 / 3 岩石 / 4 四段拼接
    float ssr_scale = 0.5f;          // 1/N
    float rough_to_lod = 8.0f;
    float rough_cutoff = 0.6f;

    // ── 六项「遮丑」能力（2026-11-13）：默认全关 ⇒ 下面 5 个老组合逐字节不变 ──
    int   max_ray_px = 220;          // SSR 像素
    float thickness_px = 6.0f;       // SSR 像素（面板默认 6）
    float fade_budget_px = 0.0f;     // 全分辨率屏幕像素，0 = 关
    float fade_edge_px = 0.0f;
    float fade_hit_edge_px = 0.0f;
    bool  filter_hole = false;
    bool  filter_clamp = false;
    bool  filter_atrous = false;
    float clamp_k = 2.0f;
    int   atrous_iters = 1;
    // 「这一项真的起作用」的下限：该组合 ON 与「六项全关」的显著变化像素占比必须 ≥ 它。
    // 0 = 不做这项断言（5 个老组合）。取值 ≈ 实测的 40%~50%，留出 llvmpipe 波动余量。
    float min_ab_ratio = 0.0f;
};

// 0.0013333 ≈ asin(0.06 / 45)：相机离地 6 cm、距原点 45 m（真正的掠射）。
inline constexpr SsrGoldCase kCases[11] = {
    {"graze_mirror", /*phi*/ 0.0013333f, /*theta*/ 0.6f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 0.5f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f},
    {"mirror_1x", /*phi*/ 0.21f, /*theta*/ 0.15f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 1.0f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f},
    {"metal_mid", /*phi*/ 0.35f, /*theta*/ 0.9f, /*R*/ 32.0f,
     /*sun*/ 25.0f, 40.0f, /*turb*/ 4.0f, /*floor*/ 1, /*scale*/ 0.5f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f},
    {"ladder_high", /*phi*/ 0.55f, /*theta*/ 0.3f, /*R*/ 30.0f,
     /*sun*/ 45.0f, 120.0f, /*turb*/ 2.0f, /*floor*/ 4, /*scale*/ 0.5f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f},
    {"dawn_metal_third", /*phi*/ 0.18f, /*theta*/ 1.2f, /*R*/ 40.0f,
     /*sun*/ 8.0f, 75.0f, /*turb*/ 3.0f, /*floor*/ 1, /*scale*/ 1.0f / 3.0f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f},

    // ── 六项能力的「各自见效」机位（2026-11-13）──
    // 每个组合只打开一个开关，其余保持默认；相机/预算都是为「让这一项真的起作用」挑的。
    // 括号里是当时实测的「相对全关」变化像素数（1280×720，>4/255），生成时核对过。

    // ① 预算淡出：把预算压到 90 SSR 像素 ⇒ 大量命中落在淡出带里（σ > 30）。
    {"fade_budget", /*phi*/ 0.21f, /*theta*/ 0.15f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 1.0f, 8.0f, 0.6f,
     /*max_ray*/ 90, /*thick*/ 6.0f, /*budget*/ 60.0f, 0.0f, 0.0f, false, false, false, 2.0f, 1, /*minAB*/ 0.006f},

    // ② 反射体贴边（P）：地板铺满下半屏 + 160 px 缓冲带 ⇒ 画面底/左边缘一圈变淡。
    {"fade_edge_p", /*phi*/ 0.21f, /*theta*/ 0.15f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 1.0f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f, 0.0f, /*edge*/ 160.0f, 0.0f, false, false, false, 2.0f, 1, /*minAB*/ 0.006f},

    // ③ 命中点贴边（Q）：宽 300 px ⇒ 映到画面左右外沿的倒影变淡（和 ② 落点不同）。
    {"fade_hit_edge_q", /*phi*/ 0.21f, /*theta*/ 1.2f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 1.0f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f, 0.0f, 0.0f, /*hit edge*/ 300.0f, false, false, false, 2.0f, 1, /*minAB*/ 0.002f},

    // ④ 补洞：1:4 降采样 + 树/桌的稀疏反射 ⇒ 梳齿缝最多。
    {"filter_hole", /*phi*/ 0.10f, /*theta*/ 0.9f, /*R*/ 30.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 0.25f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 3.0f, 0.0f, 0.0f, 0.0f, /*hole*/ true, false, false, 2.0f, 1, /*minAB*/ 0.0004f},

    // ⑤ 离群抑制：同样 1:4（噪点最多），k=1 让「只夹离群点」也看得出来。
    {"filter_clamp", /*phi*/ 0.21f, /*theta*/ 0.6f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 0.25f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f, 0.0f, 0.0f, 0.0f, false, /*clamp*/ true, false, /*k*/ 0.5f, 1, /*minAB*/ 0.0006f},

    // ⑥ 边缘保持小波：1:2 + 1 趟（默认档）—— 就是这个 PR 交付的默认观感。
    {"filter_atrous", /*phi*/ 0.21f, /*theta*/ 0.6f, /*R*/ 45.0f,
     /*sun*/ 35.0f, 60.0f, /*turb*/ 2.0f, /*floor*/ 0, /*scale*/ 0.5f, 8.0f, 0.6f,
     /*max_ray*/ 220, /*thick*/ 6.0f, 0.0f, 0.0f, 0.0f, false, false, /*atrous*/ true, 2.0f, 1, /*minAB*/ 0.006f},
};

inline std::string AssetPath(const std::string& rel) {
    return jpov::GetModelsDir() + "/" + rel;
}

inline float ScaleToHeight(const jpov::GltfObject& o, float target_h) {
    if (!o.bounds_valid) {
        return 1.0f;   // 与查看器一致：无 bounds 时不自动归一
    }
    const float h = o.bounds_max[1] - o.bounds_min[1];
    if (h <= 1.0e-6f) {
        return 1.0f;
    }
    return target_h / h;
}

// 装配「场景1」的静态资源（与 demo 的 InstallScene/InstallModels/InstallPerson 同源）。
// Pre-condition: app.Init() 已调用。
inline void AssembleScene(jpov_skylight::SkylightApp& app) {
    app.SetShowPanel(false);
    app.scene_index_ = 1;

    app.box_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf));
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.ground_strip_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad(
        jpov_skylight::kLadderStripHalfWidth, jpov_skylight::kLadderStripHalfDepth));
    app.mat_low_ = jpov_skylight::MaterialLowGloss();
    app.mat_high_ = jpov_skylight::MaterialHighGloss();
    app.mat_metal_ = jpov_skylight::MaterialMetal();
    app.mat_ground_ = jpov_skylight::GroundMaterial();
    app.mat_clutter_ = jpov_skylight::MirrorClutterMaterial();
    app.mat_mirror_skin_ = jpov_skylight::MirrorSkinMaterial();
    app.BuildClutter(/*count=*/24);

    // 桌子 / 橡树（与查看器同一位置与目标高度）。
    {
        jpov::GltfObject table = app.LoadGltf(AssetPath("scene/scene_assets/table.glb"));
        CHECK(!table.empty()) << "LoadGltf failed: table.glb";
        const float s = ScaleToHeight(table, 0.75f);
        app.AddModel(std::move(table), /*center*/ {-2.6f, 0.0f, -0.4f},
                     /*up*/ {0, 1, 0}, /*front*/ {1, 0, 0}, s);
    }
    {
        jpov::GltfObject oak = app.LoadGltf(AssetPath("samples/oak_negative/tripo_oak_4k.glb"));
        CHECK(!oak.empty()) << "LoadGltf failed: tripo_oak_4k.glb";
        const float s = ScaleToHeight(oak, 6.0f);
        app.AddModel(std::move(oak), /*center*/ {3.2f, 0.0f, -0.6f},
                     /*up*/ {0, 1, 0}, /*front*/ {-1, 0, 0}, s);
    }

    // 蓝人：一份 mesh + 一份骨架；既给方块顶面（本场景不画方块，但复用同一份资源），
    // 也给"镜面皮肤的 4 个随机站位"（曲面反射体）。
    {
        const std::string male = AssetPath("characters/mixamo_male.glb");
        jpov::GltfObject obj = app.LoadGltf(male);
        CHECK(!obj.empty() && !obj.primitives.empty()) << "LoadGltf failed: " << male;
        CHECK(obj.bounds_valid) << "蓝人资产无包围盒: " << male;
        const float asset_h = obj.bounds_max[1] - obj.bounds_min[1];
        CHECK_GT(asset_h, 1.0e-6f);
        const float scale = jpov_skylight::kPersonTargetHeight / asset_h;
        const float min_y = obj.bounds_min[1];

        app.person_mesh_ = obj.primitives[0].mesh_id;
        app.person_material_ = obj.primitives[0].material;
        CHECK_NE(app.person_mesh_, 0u);

        std::vector<jpov::SkeletonType> skins;
        CHECK(jpov::LoadGltfSkeleton(male, &skins) && !skins.empty())
            << "LoadGltfSkeleton 失败: " << male;
        jpov::SkeletonType type = skins[0];
        type.Validate();
        app.person_skeleton_id_ = app.RegisterSkeleton(
            type, {jpov::SkeletonPose::Identity(type.bone_count())});

        app.person_scale_ = scale;
        app.person_min_y_ = min_y;
        app.BuildMirrorPeople(/*count=*/4);

        app.person_gltf_ = std::move(obj);   // 资源保活
    }
}

// 把一帧的自变量写进 app（RunOnce 之前调用）。
inline void ApplyCase(jpov_skylight::SkylightApp& app, const SsrGoldCase& c) {
    app.scene_index_ = 1;
    app.floor_preset_ = c.floor_preset;
    app.view_.phi = c.view_phi;
    app.view_.theta = c.view_theta;
    app.view_.R = c.view_R;
    app.deg_.turbidity = c.turbidity;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = c.sun_elev_deg;
    app.deg_.sun_azim_deg = c.sun_azim_deg;
    app.deg_.moon_red = 0.0f;
    app.deg_.night_blue = 0.0f;
    app.deg_.elev_fog_on = false;
    app.SetEnvSpecular(true, 1.0f);
    app.ssr_scale_ = c.ssr_scale;
    app.ssr_rough_to_lod_ = c.rough_to_lod;
    app.ssr_rough_cutoff_ = c.rough_cutoff;
    app.ssr_debug_ = 0;
    // 六项能力：逐 case 显式写死（默认全关 ⇒ 老组合不受影响）。
    app.ssr_max_ray_px_ = c.max_ray_px;
    app.ssr_thickness_px_ = c.thickness_px;
    app.ssr_fade_budget_px_ = c.fade_budget_px;
    app.ssr_fade_edge_px_ = c.fade_edge_px;
    app.ssr_fade_hit_edge_px_ = c.fade_hit_edge_px;
    app.ssr_filter_hole_ = c.filter_hole;
    app.ssr_filter_clamp_ = c.filter_clamp;
    app.ssr_filter_atrous_ = c.filter_atrous;
    app.ssr_filter_clamp_k_ = c.clamp_k;
    app.ssr_filter_atrous_iters_ = static_cast<float>(c.atrous_iters);
    app.SetSsr(true);
}

// 把六项能力**全部关掉**（相机/预算/材质不动）—— 给「这一项到底起没起作用」做对照。
// Pre-condition: app 已经 ApplyCase 过（这里只改这六个开关）。
inline void DisableAllFeatures(jpov_skylight::SkylightApp& app) {
    app.ssr_fade_budget_px_ = 0.0f;
    app.ssr_fade_edge_px_ = 0.0f;
    app.ssr_fade_hit_edge_px_ = 0.0f;
    app.ssr_filter_hole_ = false;
    app.ssr_filter_clamp_ = false;
    app.ssr_filter_atrous_ = false;
}

inline std::string GoldFileName(const SsrGoldCase& c) {
    return std::string("ssr_gold_") + c.name + "_1280x720.png";
}

}  // namespace jpov_ssr_gold

#endif  // JPOV_TEST_SSR_JPOV_SSR_GOLD_COMMON_H_

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
// 环境镜面 A/B（--capture-env <out_dir>）：固定机位只切“天光消费量”（0 / 0.5 / 1），
//   验收工件 = 第三方块（金属，metallic=1）—— 关时发黑，开时能看见天光渐变与地色。
// 镜面高光带宽（--capture-mirror-specular <out_dir>）：地面换成 roughness=0 的大镜面，
//   机位算死使太阳的像落在画面正中 —— 验收「δ 光源 × δ BRDF」的带宽（docs/jpov_specular_bandwidth.md）。
// 点光源镜面带宽（--capture-point-specular <out_dir>）：同场景换成“只有一盏球灯”，
//   灯放在原点处的反射方向上使它的像落在画面正中 —— 扫 physical_radius 看像变大变柔。
//
// 架构：与 model viewer 同构（主程序只做装配 + 事件循环；场景/天光装配在
// skylight_scene.h，渲染核心 + 面板在 skylight_viewer_app.h）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_skylight_viewer
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_skylight_viewer.sh
//   → output/jpov_skylight_viewer/jpov_skylight_viewer

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/skylight_viewer_app.h"
#include "tools/jpov/demo/viewer_render_preference.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/common/utils.h"

namespace {

// 装配场景静态资源（交互与 headless 共用，避免两处各写一套而分叉）。
void InstallScene(jpov_skylight::SkylightApp& app) {
    app.box_mesh_    = app.RegisterMesh(jpov::MeshData::MakeBox(
        jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf,
        jpov_skylight::kBoxHalf));
    app.ground_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad());
    app.ground_strip_mesh_ = app.RegisterMesh(jpov_skylight::MakeGroundQuad(
        jpov_skylight::kLadderStripHalfWidth, jpov_skylight::kLadderStripHalfDepth));
    app.mat_low_    = jpov_skylight::MaterialLowGloss();
    app.mat_high_   = jpov_skylight::MaterialHighGloss();
    app.mat_metal_  = jpov_skylight::MaterialMetal();
    app.mat_ground_ = jpov_skylight::GroundMaterial();
    app.mat_clutter_ = jpov_skylight::MirrorClutterMaterial();
    app.mat_mirror_skin_ = jpov_skylight::MirrorSkinMaterial();
    app.BuildClutter(/*count=*/24);
}

// 额外模型（桌子 / 高模橡树）的**相对路径**（相对 exe 所在目录）。
// build_jpov_skylight_viewer.sh 把两个 glb 拷到 output/<demo>/models/ 下；
// 分发态即“exe 旁 models/” —— 与字体同一套“拷贝法”惯例（SOUL：路径硬编码 + 脚本拷贝）。
inline constexpr const char* kTableModelPath = "models/table.glb";
inline constexpr const char* kOakModelPath   = "models/tripo_oak_4k.glb";

// 蓝人（T-pose 人形）模型：带骨架 + 蒙皮通道（JOINTS_0/WEIGHTS_0）。
// build 脚本把 mixamo_male.glb 拷到 exe 旁 models/（同其它模型一套“拷贝法”惯例）。
inline constexpr const char* kMaleModelPath  = "models/mixamo_male.glb";

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

// 蓝人（mixamo_male）：一份蒙皮 mesh + 一份骨架（单 identity pose = T-pose），
// 每个方块顶面各摆一个实例 —— 三人共用同一份几何/骨架，只差摆放，**一次 instanced
// draw call** 画完（见 SkylightApp::OneIteration）。
//
// 高度/落地归一（不假设资产坐标系）：scale = 目标身高 / 资产包围盒 Y 高；脚点对齐方块
// 顶面（center.y = 顶面 − scale·资产 min.y），因此“脚贴在方块上”与资产原点在哪无关。
void InstallPerson(jpov_skylight::SkylightApp& app) {
    jpov::GltfObject obj = app.LoadGltf(kMaleModelPath);
    CHECK(!obj.empty()) << "LoadGltf failed: " << kMaleModelPath
                        << "（分发态需 build 脚本把模型拷到 exe 旁 models/）";
    CHECK(!obj.primitives.empty());
    CHECK(obj.bounds_valid) << "蓝人资产无包围盒，无法归一高度: " << kMaleModelPath;
    const float asset_h = obj.bounds_max[1] - obj.bounds_min[1];
    CHECK_GT(asset_h, 1.0e-6f) << "蓝人资产 Y 向高度≈0: " << kMaleModelPath;
    const float scale = jpov_skylight::kPersonTargetHeight / asset_h;
    const float min_y = obj.bounds_min[1];

    app.person_mesh_     = obj.primitives[0].mesh_id;
    app.person_material_ = obj.primitives[0].material;
    CHECK_NE(app.person_mesh_, 0u) << "蓝人真皮网格句柄为 0（加载失败）";

    // 骨架：同一 glb 的 skin[0]；只烘培一帧 identity pose（= rest/T-pose）。
    //   ⚠️ LoadGltfSkeleton 是**纯 loader**，不解析相对路径（不认 exe 旁 / TEST_SRCDIR）；
    //   而上面的 app.LoadGltf 走 Renderer::LoadGltf 已经解析过。这里用同一套
    //   ResolveResourcePath 手动解析，否则分发态下会“文件找不到”（实测血泪）。
    const std::string skel_path = jpov::ResolveResourcePath(kMaleModelPath);
    std::vector<jpov::SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(skel_path, &skins) && !skins.empty())
        << "LoadGltfSkeleton 失败或无 skin: " << skel_path;
    jpov::SkeletonType type = skins[0];
    type.Validate();
    app.person_skeleton_id_ = app.RegisterSkeleton(
        type, {jpov::SkeletonPose::Identity(type.bone_count())});

    // 三个站位：每个方块顶面一个（同一份 mesh/骨架/pose，只差 transform）。
    app.person_instances_.clear();
    app.person_instances_.reserve(3);
    for (int i = 0; i < 3; ++i) {
        const jpov::Vec3f top = jpov_skylight::BoxTopCenter(i);
        jpov::SkinnedInstanceState inst;
        inst.transform.center = {top.x(), top.y() - scale * min_y, top.z()};
        inst.transform.up     = {0.0f, 1.0f, 0.0f};
        inst.transform.front  = {0.0f, 0.0f, 1.0f};
        inst.transform.scale  = scale;
        inst.pose_a = 0;   // 唯一一帧 = identity = T-pose
        inst.pose_b = 0;
        inst.ratio  = 0.0f;
        app.person_instances_.push_back(inst);
    }
    // 镜面皮肤的蓝人（曲面 SSR 反射体）：复用同一份缩放/脚点，随机摆 4 个。
    app.person_scale_ = scale;
    app.person_min_y_ = min_y;
    app.BuildMirrorPeople(/*count=*/4);

    app.person_gltf_ = std::move(obj);   // 资源保活（释放走 Finalize）

    LOG(INFO) << "blue-man assembled: mesh_id=" << app.person_mesh_
              << " skeleton_id=" << app.person_skeleton_id_
              << " bones=" << type.bone_count()
              << " asset_h=" << asset_h << " scale=" << scale;
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
    InstallPerson(app);

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

    // ── G. 三色环境光对比：**同机位/同自由度**，只切 ambient 三色开关 ──
    // 正午（看“天顶冷蓝 vs 地平”梯度）、低斜阳（暖地平端）与夜间（看夜色量级是否与单色对齐）。
    for (int e : {60, 12, -30}) {
        app.deg_.sun_elev_deg = static_cast<float>(e);
        app.SetTricolorAmbient(false);
        shoot(("ambient_single_elev" + std::to_string(e)).c_str());
        app.SetTricolorAmbient(true);
        shoot(("ambient_tricolor_elev" + std::to_string(e)).c_str());
    }
    app.SetTricolorAmbient(false);

    app.Finalize();
    return 0;
}

// SMAA A/B 拍摄：固定机位/光照，只切 SMAA 开关，逐个出图供肉眼对比。
// 用法：jpov_skylight_viewer --capture-smaa <out_dir> [--smaa-threshold <t>]
// 出图：smaa_off.png / smaa_on.png（阈值默认 0.1，可由 --smaa-threshold 覆盖）。
int RunCaptureSmaa(const std::string& out_dir, float threshold) {
    JPOV::Config cfg;
    cfg.title = "JPOV — SMAA A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 固定一组观感：正午侧光，中等距离——方块棱边 / 人形轮廓 / 树叶边缘都入画，
    // 是观察锯齿最有代表性的机位。
    app.deg_.turbidity    = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 35.0f;
    app.deg_.sun_azim_deg = 60.0f;
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.22;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 6.5;

    // 关 → 开：同一机位只切 SMAA。
    app.SetSmaa(false);
    app.RunOnce(input, winfo, (out_dir + "/smaa_off.png").c_str());
    app.SetSmaa(true, threshold);
    app.RunOnce(input, winfo, (out_dir + "/smaa_on.png").c_str());

    LOG(INFO) << "SMAA A/B captured to " << out_dir
              << " (threshold=" << threshold << ")";
    app.Finalize();
    return 0;
}

// 环境镜面 A/B 拍摄：固定机位/光照，只切“天光消费量”。
// 用法：jpov_skylight_viewer --capture-env <out_dir>
// 出图：env_off.png（关闭）/ env_half.png（0.5）/ env_on.png（1.0）
//
// 为什么用这三个档：
//   off ↔ on  = 验收“这一块到底做了什么”（金属块从发黑 → 有天空反射）；
//   half      = 验收“乘子真的是连续可调”，且半量看起来应当位于两者之间。
int RunCaptureEnv(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 环境镜面 A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 固定观感：正午侧光（让金属块同时照到天空与太阳，反射方向不恰好垂直向下），
    // 机位拉近到三方块，金属块（右侧）占屏面积够大。
    app.deg_.turbidity    = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 40.0f;
    app.deg_.sun_azim_deg = 30.0f;
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.20;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 5.2;

    app.SetEnvSpecular(false);
    app.RunOnce(input, winfo, (out_dir + "/env_off.png").c_str());
    app.SetEnvSpecular(true, 0.5f);
    app.RunOnce(input, winfo, (out_dir + "/env_half.png").c_str());
    app.SetEnvSpecular(true, 1.0f);
    app.RunOnce(input, winfo, (out_dir + "/env_on.png").c_str());

    LOG(INFO) << "env specular A/B captured to " << out_dir
              << " (env_off / env_half / env_on)";
    app.Finalize();
    return 0;
}

}  // namespace

// 水体介质（水凝胶）A/B 拍摄：固定机位/光照，只切介质开关与岩板深度。
// 用法：jpov_skylight_viewer --capture-water <out_dir>
//
// 出图（同一机位；前五张是“凝胶对物体的遮挡”，后两张是“对天色的吸收”）：
//   water_off.png      介质关 —— 岩板/三件方块的原色（对照基线）
//   water_on_d2.png    介质开，岩板深度 2 m
//   water_on_d5.png    5 m
//   water_on_d10.png   10 m
//   water_on_d20.png   20 m（红通道应该基本没了，只剩蓝）
//   water_sky_off.png  无几何 + 介质关：天空下半球的原始地色
//   water_sky_on.png   无几何 + 介质开：整段水柱把天色吃掉 + 注入自身颜色
//
// 为什么这组就够：
//   · 遮挡 = **逐通道衰减**：岩板是中性灰材质，颜色只会在灰度线上变暗 + 向蓝偏；
//   · 天色吸收：射线向下穿过整段水体到远平面 ⇒ 背景 = 地平线以下天空色被 exp(−τ) 吞掉，
//     剩下的只能是介质自己的注入项（σs 越大越亮）。
int RunCaptureWater(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水体介质（水凝胶）A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 机位 A（俯视 35°）：看“凝胶对物体的遮挡”—— 岩板 + 三件方块。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.62;
    app.view_.theta = 0.6;
    app.view_.R     = 26.0;
    // 太阳：侧上方，能看出“逐通道吸收 + 相位方向性”的差别。
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;

    // 机位 B（近水平 9°）：看“凝胶对天色的吸收”—— 地平线以上是空气里的天，
    // 以下要穿过整段水体 ⇒ 天色被 exp(−τ) 吞掉，只剩介质自己的注入色。
    const auto setCameraB = [&app]() {
        app.view_.phi   = 0.16;
        app.view_.theta = 0.6;
        app.view_.R     = 30.0;
    };

    app.SetWaterMedium(false);
    app.RunOnce(input, winfo, (out_dir + "/water_off.png").c_str());

    app.SetWaterMedium(true);
    app.SetSlabDepth(2.0f);
    app.RunOnce(input, winfo, (out_dir + "/water_on_d2.png").c_str());
    app.SetSlabDepth(5.0f);
    app.RunOnce(input, winfo, (out_dir + "/water_on_d5.png").c_str());
    app.SetSlabDepth(10.0f);
    app.RunOnce(input, winfo, (out_dir + "/water_on_d10.png").c_str());
    app.SetSlabDepth(20.0f);
    app.RunOnce(input, winfo, (out_dir + "/water_on_d20.png").c_str());

    // ── 水下两张（M2 水皮验收）──
    // (a) 水下俯视：把**水位**抬到 +6 m（相机 4.2 m ⇒ 水下 1.8 m），目标仍是原点 ⇒ 看下去。
    //     岩板顶面 = 水位 − 5 m = +1 m（水面下 5 m）。
    app.SetShowScene(true);
    app.SetWaterLevel(6.0f);
    app.SetSlabDepth(5.0f);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.62f;
    app.view_.theta = 0.6f;
    app.view_.R     = 26.0f;
    app.RunOnce(input, winfo, (out_dir + "/water_under_down.png").c_str());

    // (a2) 半浸方块：水位 +6、岩板深度 0.5 ⇒ 岩板顶面 +5.5 m（水面下 0.5 m），
    //      2.2 m 高的方块露出水面 —— 用来看「水线上下的物体」的地色/反射是否各归各位。
    app.SetWaterLevel(6.0f);
    app.SetSlabDepth(0.5f);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.55f;
    app.view_.theta = 0.6f;
    app.view_.R     = 16.0f;
    app.RunOnce(input, winfo, (out_dir + "/water_halfbox.png").c_str());

    // (b) 水下抬头：水位回 0、相机在原点下方（phi<0）⇒ 直视上方的 Snell 窗与 TIR 环。
    app.SetWaterLevel(0.0f);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = -0.60f;
    app.view_.theta = 0.70f + 3.14159265f;
    app.view_.R     = 8.0f;
    app.RunOnce(input, winfo, (out_dir + "/water_under_up.png").c_str());

    // 天色吸收：不画任何几何 + 近水平机位 ⇒ 上半屏是空气里的天，下半屏是“被水柱吃掉的天色”。
    app.SetSlabDepth(jpov_skylight::kSlabTopDepthDefM);
    app.SetShowScene(false);
    setCameraB();
    app.SetWaterMedium(false);
    app.RunOnce(input, winfo, (out_dir + "/water_sky_off.png").c_str());
    app.SetWaterMedium(true);
    app.RunOnce(input, winfo, (out_dir + "/water_sky_on.png").c_str());

    LOG(INFO) << "water medium A/B captured to " << out_dir
              << " (water_off / on_d2,5,10,20 / sky_off,on)";
    app.Finalize();
    return 0;
}

// 地平线暗线复现：**不碰 deg_（用交互默认光照）**，波幅 0，只扫机位。
// 用法：jpov_skylight_viewer --capture-horizon-repro <out_dir>
//
// 为什么要单独一个入口：`--capture-water-waves` 会把太阳压到 18°/214°（为了拍日盘像），
// 而用户报障时用的是**默认光照** —— 地平线天空亮度和 env 探针内容都完全不同，
// 照搬那组参数是复现不出来的。
int RunCaptureHorizonRepro(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 地平线暗线复现（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.SetWaterMedium(true);
    app.SetSlabDepth(2.0f);
    app.SetWaterWaves(true);
    app.SetWaterWindSpeed(0.0f);   // 静水（= 风速 0）

    LOG(INFO) << "repro: default deg_ = elev " << app.deg_.sun_elev_deg
              << " azim " << app.deg_.sun_azim_deg
              << " turb " << app.deg_.turbidity;

    struct View { const char* name; float phi; float theta; float radius; };
    const View kViews[6] = {
        {"/repro_1_ww.png",   0.22f, 0.6f,           18.0f},   // --water-waves 机位
        {"/repro_2_default.png", 0.25f, 0.7853981634f, 9.5f},  // 交互启动默认
        {"/repro_3.png",      0.30f, 0.6f,           20.0f},
        {"/repro_4.png",      0.15f, 0.6f,           12.0f},
        {"/repro_5.png",      0.35f, 0.9f,           25.0f},
        {"/repro_6.png",      0.45f, 1.2f,           30.0f},
    };
    for (const View& v : kViews) {
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi = v.phi;
        app.view_.theta = v.theta;
        app.view_.R = v.radius;
        app.RunOnce(input, winfo, (out_dir + v.name).c_str());
    }

    // ── Danis 的实际机位（从他截图左上调试面板读出来）──
    //   CAM pos (15.4197826, 4.23410654, -8.26453876) -> (0,0,0), fov 60, near 0.05
    //   ⇒ phi = asin(4.2341/18) ≈ 0.23743, theta = atan2(x, z) ≈ 2.0629, R = 18.0
    // 他是在 --water-waves 基础上把相机横向拖了、然后在 UI 里把「水面波浪」关掉。
    {
        const float kPhi = 0.27236f;    // CAM pos (8.8565, 4.8421, 14.9035) -> 原点
        const float kTheta = 0.536186f;
        const float kR = 18.0f;
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi = kPhi;
        app.view_.theta = kTheta;
        app.view_.R = kR;
        app.SetWaterWaves(true);
        app.RunOnce(input, winfo, (out_dir + "/repro_his_waves_on.png").c_str());
        app.SetWaterWaves(false);   // ← 他在 UI 里关掉了波浪
        app.RunOnce(input, winfo, (out_dir + "/repro_his_waves_off.png").c_str());
        // debug：alpha / t（波幅 0 = 与关掉等价，但能让 debug 透传）
        app.SetWaterWaves(true);
        app.SetWaterWindSpeed(0.0f);   // 静水（= 风速 0）
        app.SetWaterWaveDebug(6);
        app.RunOnce(input, winfo, (out_dir + "/repro_dbg_cover.png").c_str());
        // 二分：关掉**水体介质**（保留水皮）——暗线还在=反射/探针；消失=介质合成
        app.SetWaterMedium(false);
        app.RunOnce(input, winfo, (out_dir + "/repro_no_medium.png").c_str());
        app.SetWaterMedium(true);
        app.SetWaterWaveDebug(4);
        app.RunOnce(input, winfo, (out_dir + "/repro_dbg_alpha.png").c_str());
        app.SetWaterWaveDebug(5);
        app.RunOnce(input, winfo, (out_dir + "/repro_dbg_t.png").c_str());
        app.SetWaterWaveDebug(0);
        app.SetWaterWindSpeed(jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    }

    // 对照：同一机位、**把水整个关掉**（没有 WaterCommand）。
    // 地平线那几行的像素 = 天空在地平线以下填的 ground 色；用来判断「暗线」是不是
    // 「水面缺失、露出地色」。
    app.SetWaterScene(false);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = 0.27236f;
    app.view_.theta = 0.536186f;
    app.view_.R = 18.0f;
    app.RunOnce(input, winfo, (out_dir + "/repro_skyonly.png").c_str());
    app.SetWaterScene(true);

    // ── 水下仰视：Snell 窗（天球折射消费波浪法线的验收）──
    // 水位抬到 +6 ⇒ 相机 y≈-3.13（水下 9.1 m），抬头看水面：窗内是压缩的天空，
    // 窗外是全内反射。关波浪时窗缘是死圆；开波浪时窗缘应该随法线起伏。
    app.SetWaterLevel(6.0f);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = -0.55f;
    app.view_.theta = 0.7f + 3.14159265f;
    app.view_.R = 6.0f;
    app.SetWaterWaves(false);
    app.RunOnce(input, winfo, (out_dir + "/under_snell_waves_off.png").c_str());
    app.SetWaterWaves(true);
    app.SetWaterWindSpeed(1.5f * jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.RunOnce(input, winfo, (out_dir + "/under_snell_waves_on.png").c_str());
    // （水下不能关介质：renderer 有 CHECK，"水下必须开 medium"否则环外 α=1 会盖成黑环）
    app.SetWaterWindSpeed(jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.SetWaterLevel(0.0f);

    // ── Danis 报障机位：贴水面仰视（cam.y = -0.438，水面下 0.44 m），时间冻结 43.82 s ──
    app.SetWaterLevel(0.0f);
    app.SetWaterWaveTime(43.82f);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi = -0.0244f;
    app.view_.theta = 3.9973f;
    app.view_.R = 18.0f;
    app.SetWaterWaves(true);
    app.RunOnce(input, winfo, (out_dir + "/bug_shallow_wave_on.png").c_str());
    // debug：alpha / wave_ok / cos_i（同一机位 + 同一冻结时间）
    for (int dbg : {7, 8, 9}) {
        app.SetWaterWaveDebug(dbg);
        char nm[64];
        std::snprintf(nm, sizeof(nm), "/bug_dbg_%d.png", dbg);
        app.RunOnce(input, winfo, (out_dir + nm).c_str());
    }
    app.SetWaterWaveDebug(0);
    app.SetWaterWaves(false);
    app.RunOnce(input, winfo, (out_dir + "/bug_shallow_wave_off.png").c_str());
    app.SetWaterWaves(true);

    LOG(INFO) << "horizon repro captured to " << out_dir;
    app.Finalize();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 水面覆盖 / 合成验收（2026-11-19 三处修改的回归拍摄）
// 用法：jpov_skylight_viewer --capture-water-coverage <out_dir>
//
// 出图：
//   cov_1_default.png            他的机位（pos -181.7,141.3,-192.3 / 太阳 +4°）全开
//   cov_2_skin_off.png           同上，关水皮 —— 验证「介质铺到地平线、无地色带」
//   cov_3_medium_off.png         同上，关介质
//   cov_4_horizon_cam.png        他的另一个机位（pos -172.8,15.8,-219.4 / 太阳 2°）全开
//   cov_5_horizon_cam_sky.png    同上，完全没有水（天球自己的地平线对照）
//
// 历史背景（都写进 known_issue）：
//   · 关水皮时地平线以下曾露「天球地色」⇒ 介质积到无穷远（z_med = +inf）
//   · 低太阳远机位曾在 x≈383/895 出现竖直硬边 ⇒ 合成趟按「水皮包住水体」处理前缀
int RunCaptureWaterCoverage(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水面覆盖/合成验收（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };
    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 4.0f;
    app.deg_.sun_azim_deg    = 45.0f;
    app.SetWaterMedium(true);
    app.SetWaterSkin(true);
    app.SetWaterWaves(true);
    app.SetWaterWindSpeed(jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.SetWaterLevel(0.0f);

    // 机位 A：太阳贴地平线（+4°）、远机位 —— 合成硬边就出在这里
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.4907;    // pos (-181.745285, 141.326981, -192.341721)
    app.view_.theta = -2.3844;
    app.view_.R     = 300.0;
    app.RunOnce(input, winfo, (out_dir + "/cov_1_default.png").c_str());
    app.SetWaterSkin(false);
    app.RunOnce(input, winfo, (out_dir + "/cov_2_skin_off.png").c_str());
    app.SetWaterSkin(true);
    app.SetWaterMedium(false);
    app.RunOnce(input, winfo, (out_dir + "/cov_3_medium_off.png").c_str());
    app.SetWaterMedium(true);

    // 机位 B：贴水面（2° 太阳）—— 覆盖/地平线对齐
    app.deg_.sun_elev_deg = 2.0f;
    app.view_.phi   = 0.05658142;   // pos (-172.770751, 15.819922, -219.447536)
    app.view_.theta = -2.47464456;
    app.view_.R     = 279.744926;
    app.RunOnce(input, winfo, (out_dir + "/cov_4_horizon_cam.png").c_str());
    app.SetWaterScene(false);       // 无水对照：天球自己的地平线
    app.RunOnce(input, winfo, (out_dir + "/cov_5_horizon_cam_sky.png").c_str());
    app.SetWaterScene(true);

    LOG(INFO) << "water coverage captured to " << out_dir;
    app.Finalize();
    return 0;
}

// 地平线遮挡回归（Danis 2026-11-19 16:41 报的「box 被水皮锯」）：
// 近水面机位，箱子上沿被水面盖掉 3 行。根因是水皮对「地平线钳位」像素整条跳过深度测试。
// 用法：jpov_skylight_viewer --capture-box-cut <out_dir>
//
//   box_1_default.png   全开（箱子应当完整）
//   box_2_skin_off.png  关水皮（对照：箱子本来长这样）
//   box_3_waves_off.png 关波浪
int RunCaptureBoxCut(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — box 被水皮锯（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };
    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.SetWaterMedium(true);
    app.SetWaterSkin(true);
    app.SetWaterWaves(true);
    app.SetWaterWindSpeed(jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.SetWaterLevel(0.0f);

    // 他 16:41 截图：pos (-8.6907061, 0.40962684, -2.30289817) -> (0,0,0), fov 60
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.04552;
    app.view_.theta = -1.8299;
    app.view_.R     = 9.0;

    app.RunOnce(input, winfo, (out_dir + "/box_1_default.png").c_str());
    app.SetWaterSkin(false);
    app.RunOnce(input, winfo, (out_dir + "/box_2_skin_off.png").c_str());
    app.SetWaterSkin(true);
    app.SetWaterWaves(false);
    app.RunOnce(input, winfo, (out_dir + "/box_3_waves_off.png").c_str());
    app.SetWaterWaves(true);

    LOG(INFO) << "box cut captured to " << out_dir;
    app.Finalize();
    return 0;
}

// 水面波浪（法线扰动/高度置换）A/B：同一机位/光照/介质，只切**波幅倍率**。
// 用法：jpov_skylight_viewer --capture-water-waves <out_dir>
//
// 出图（frame_count 恒为 0 ⇒ time_s = 0，所以四张只有波幅不同，是干净的 A/B）：
//   water_waves_off.png   波幅 0（求交仍走 Newton，但 h≡0 ⇒ 应等于纯平面）
//   water_waves_low.png   0.6×
//   water_waves_mid.png   1.0×（默认谱：4 波，长 24/13/7/3.5 m，振幅 0.28/0.14/0.07/0.03 m）
//   water_waves_high.png  1.8×
//
// 验收点：反射的天光被扰动成碎光、太阳的像被拉成随波起伏的亮带；远处（fade_end 700 m）
// 回到平面。水体介质在这组里不参与波浪（契约 §11.5），所以水下吸收仍是平面口径。
int RunCaptureWaterWaves(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水面波浪（法线扰动/高度置换）（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 机位：低空俯视（phi 0.30 ≈ 17°），近处能看清波纹明暗、远处是掠射碎光。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.30f;
    app.view_.theta = 0.6f;
    app.view_.R     = 20.0f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 18.0f;   // 低太阳 ⇒ 反射路径长，波浪更容易把日盘像切碎
    // ⚠️ 日盘像只出现在「相机水平朝向 ≈ 太阳方位」的一侧：theta=0.6 ⇒ 相机朝 214.4°。
    //    取 40° 时太阳在相机背后，水面根本收不到它的像（白拍一轮）。
    app.deg_.sun_azim_deg    = 214.0f;

    app.SetWaterMedium(true);
    app.SetSlabDepth(2.0f);

    // A/B：波浪**整个关掉**（命令里不带 waves）vs 开着但波幅 0。
    // 这两张应当逐字节相同；不同就说明「波浪路径在 amp=0 时不等价于平面」。
    app.SetWaterWaves(false);
    app.RunOnce(input, winfo, (out_dir + "/debug_wave_disabled.png").c_str());
    app.SetWaterWaves(true);

    // ── 地平线暗带复现（Danis：默认参数 + 波幅 0）──
    // 三个候选机位，逐个出图后用 2D 检测（逐像素，不信行均值）找 1px 暗线：
    //   A = 交互启动默认（phi 0.25 / theta 45° / R 9.5）
    //   B = --water-waves 的机位（0.22 / 0.6 / 18）
    //   C = capture 默认（0.30 / 0.6 / 20）
    app.SetWaterWindSpeed(0.0f);   // 静水（= 风速 0）
    {
        struct Repro { const char* name; float phi; float theta; float radius; };
        const Repro kReps[3] = {
            {"/debug_repro_a.png", 0.25f, 0.7853981634f, 9.5f},
            {"/debug_repro_b.png", 0.22f, 0.6f, 18.0f},
            {"/debug_repro_c.png", 0.30f, 0.6f, 20.0f},
        };
        for (const Repro& r : kReps) {
            app.view_.phi = r.phi;
            app.view_.theta = r.theta;
            app.view_.R = r.radius;
            app.RunOnce(input, winfo, (out_dir + r.name).c_str());
        }
    }
    app.SetWaterWindSpeed(jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.view_.phi = 0.30f;
    app.view_.R = 20.0f;

    // ── 二分 debug：法线 only vs 真位移（同一波幅、同一机位）──
    //   用来定位「浪坑里的法线噪点」：若 normal_only 干净、displaced 有噪 ⇒ 求交；
    //   两者都脏 ⇒ 法线/着色。
    app.SetWaterWindSpeed(1.75f * jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.RunOnce(input, winfo, (out_dir + "/debug_normal_only.png").c_str());
    app.RunOnce(input, winfo, (out_dir + "/debug_displaced.png").c_str());
    // 法线可视化：N*0.5+0.5。连续渐变 = 法线正常；彩色椒盐 = 法线场跳变。
    app.SetWaterWaveDebug(1);
    app.RunOnce(input, winfo, (out_dir + "/debug_normal_color.png").c_str());
    app.RunOnce(input, winfo, (out_dir + "/debug_displaced_color.png").c_str());
    app.SetWaterWaveDebug(2);
    app.RunOnce(input, winfo, (out_dir + "/debug_normalvar.png").c_str());
    app.SetWaterWaveDebug(0);

    // 四档海况（静水 / 8 / 14 / 20 m/s）：后两档是**夸张档**，专门让人一眼确认
    // 「求交真的被推动了」，不是只改了法线。（2026-11-24：接口只有风速，没有"波幅倍率"。）
    const float kScales[4] = {0.0f, 8.0f, 14.0f, 20.0f};
    const char* kNames[4] = {
        "/water_waves_off.png",
        "/water_waves_low.png",
        "/water_waves_mid.png",
        "/water_waves_high.png",
    };
    for (int i = 0; i < 4; ++i) {
        app.SetWaterWindSpeed(kScales[i]);
        app.RunOnce(input, winfo, (out_dir + kNames[i]).c_str());
    }

    // 近水平掠射（phi 0.05 ≈ 2.9°）：水面几乎全反射，波纹会把天空打碎成碎银光带 ——
    // 这是「法线扰动真的在改变反射方向」最直观的一张。
    app.view_.phi   = 0.05f;
    app.view_.theta = 0.6f;
    app.view_.R     = 24.0f;
    app.SetWaterWindSpeed(1.25f * jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.RunOnce(input, winfo, (out_dir + "/water_waves_grazing.png").c_str());

    // 贴近水面（R=8）：波峰剪影遮住远处物体 —— 几何置换的直接证据（不是 bump）。
    app.view_.phi   = 0.16f;
    app.view_.theta = 0.6f;
    app.view_.R     = 8.0f;
    app.SetWaterWindSpeed(1.5f * jpov_skylight::SkylightApp::kWaterDefaultWindMs);
    app.RunOnce(input, winfo, (out_dir + "/water_waves_close.png").c_str());

    LOG(INFO) << "water waves A/B captured to " << out_dir
              << " (water_waves_off / low / mid / high)";
    app.Finalize();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 海面报告（4 合 1）：同一套配置拍 4 个固定机位，用于「像不像大海」的横向比较。
//
// 用法：jpov_skylight_viewer --capture-ocean-report <out_dir> [--ocean-config <file>]
//
// 配置是纯文本、每行一条（# 后面是注释，缺省项用内置默认）：
//   sun     <elev_deg> <azim_deg>          # 太阳方向
//   sigma_a <r> <g> <b>                     # 水体吸收 1/m
//   sigma_s <s>                             # 水体散射 1/m（demo 侧 σs 是标量）
//   wind    <m/s> [dir_deg]                 # 风：速度（可选吹向）
//   fetch_m <米>                            # 风区（池塘 10~200 m；不写 = 充分成长）
//   duration <h>                            # 风时（不写 = 充分成长）
//   swell   <hs_m> <tp_s> <dir_deg>          # 涌（不写 = 无涌）
//   depth   <米>                            # 水深（色散用）
//   ior     <值>                            # 折射率
//   level   <米>                            # 水位
//   seed    <整数>                          # 波相位随机种子
//   exposure <倍率>                         # tone map 之前的线性增益
//   turbidity <值>                          # 大气浊度（2 清澈…8 霾）
//   debug   <0|1|...>                       # 水皮 debug 档（0 = 正常）
//   speed   <倍率>                          # 色散时间倍率
//   （stokes / warp / wave / detail / fade / farfield / graze_floor 已随接口收口删除）
//
// 四个机位（固定，不可配）：
//   1) 45° 俯角 + 5 m 高（近场波纹/碎光）
//   2) 45° 俯角 + 40 m 高（中景图案/周期性）
//   3) 10° 掠射、朝向太阳（glitter 路径）
//   4) 10° 掠射、背向太阳（逆光碎光）
namespace {

struct OceanConfig {
    float sun_elev_deg = 25.0f;
    float sun_azim_deg = 214.0f;
    float turbidity = 2.0f;
    float exposure = 1.0f;
    int debug = 0;            // 1 = 法线颜色可视化（诊断用）
    float time_speed = 1.0f;  // 时间倍率
    // 水体本身 = **就是 WaterConfig**（2026-11-24）：配置文件不再能拼波表。
    jpov::water::WaterConfig water;
};

bool LoadOceanConfig(const std::string& path, OceanConfig* out) {
    CHECK(out != nullptr);
    std::ifstream in(path);
    if (!in) {
        LOG(ERROR) << "打不开海面配置: " << path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream ss(line);
        std::string tag;
        if (!(ss >> tag)) {
            continue;
        }
        if (tag == "sun") {
            ss >> out->sun_elev_deg >> out->sun_azim_deg;
        } else if (tag == "sigma_a") {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            ss >> r >> g >> b;
            out->water.sigma_a = {r, g, b};
        } else if (tag == "sigma_s") {
            // demo 侧 σs 是标量（逐通道同值）：多写的 g/b 会被忽略，所以只读第一个。
            ss >> out->water.sigma_s;
        } else if (tag == "wind") {
            float speed = 0.0f;
            ss >> speed;
            float dir_deg = out->water.WindDirRad() * 180.0f / 3.14159265358979323846f;
            if (!(ss >> dir_deg)) {
                // 只写了速度：沿用当前吹向
            }
            out->water.SetWind(speed, dir_deg * 3.14159265358979323846f / 180.0f);
        } else if (tag == "fetch_m") {
            float m = 0.0f;
            ss >> m;
            out->water.fetch_km = (m > 0.0f) ? m * 0.001f
                                             : std::numeric_limits<float>::infinity();
        } else if (tag == "duration") {
            float h = 0.0f;
            ss >> h;
            out->water.duration_h = (h > 0.0f) ? h : std::numeric_limits<float>::infinity();
        } else if (tag == "swell") {
            float hs = 0.0f;
            float tp = 9.5f;
            float dir_deg = 0.0f;
            ss >> hs >> tp >> dir_deg;
            out->water.SetSwell(hs, tp, dir_deg * 3.14159265358979323846f / 180.0f);
        } else if (tag == "depth") {
            ss >> out->water.depth_m;
        } else if (tag == "ior") {
            ss >> out->water.ior;
        } else if (tag == "level") {
            ss >> out->water.level_y;
        } else if (tag == "seed") {
            ss >> out->water.seed;
        } else if (tag == "exposure") {
            ss >> out->exposure;
        } else if (tag == "turbidity") {
            ss >> out->turbidity;
        } else if (tag == "debug") {
            ss >> out->debug;
        } else if (tag == "speed") {
            ss >> out->time_speed;
        } else if (tag == "stokes" || tag == "warp" || tag == "wave" || tag == "detail" ||
                   tag == "fade" || tag == "farfield" || tag == "graze_floor") {
            // 2026-11-24 接口收口：波表/淡出/护栏都不再是调用方参数（谱由 BuildWater 展开）。
            LOG(WARNING) << "配置项已废弃并被忽略: " << tag;
        } else {
            LOG(WARNING) << "未知海面配置项: " << tag;
        }
    }
    return true;
}

}  // namespace

// 海面选型场的验证拍摄：只换 WaterConfig 里的风/涌，机位固定 3 个。
// 交互版走同一条渲染路径 + 面板；这里只是把参数拍下来存档/做回归。
int RunCaptureOceanLab(const std::string& out_dir, bool show_ui, float wave_time_s,
                       int water_debug) {
    const float kPi = 3.14159265358979323846f;
    JPOV::Config cfg;
    cfg.title = "JPOV — 海面选型（WaterConfig，headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(show_ui);   // true = 连面板一起拍（验收 UI 布局）
    app.SetShowScene(true);
    app.Init();
    InstallScene(app);
    if (show_ui) {
        InstallModels(app);
    }
    app.InstallTextMeasure();
    app.SetOceanLab(true);
    // 冻结时刻：同一时刻下差异只来自配置（报障复现用 --ocean-time 对齐）。
    app.SetWaterWaveTime(wave_time_s);
    app.SetWaterWaveDebug(water_debug);   // 1=法线 2=像素内法线方差 11=探针 LOD

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    if (show_ui) {
        app.SetOceanLabView();
        app.RunOnce(input, winfo, (out_dir + "/oceanlab_ui.png").c_str());
        LOG(INFO) << "ocean lab UI captured to " << out_dir;
        app.Finalize();
        return 0;
    }

    // 四个配置：① 默认（风海）② 涌 ③ 大风 + 小风区（年轻海）④ 平静
    struct Preset {
        const char* name;
        float wind_ms;
        float wind_dir_deg;
        float fetch_km;
        float swell_hs;
        float swell_tp;
        float swell_dir_deg;
    };
    const Preset kPresets[5] = {
        {"a_windsea",   8.0f,  34.0f, 250.0f, 0.0f, 9.5f,  0.0f},
        {"b_wind_swell", 8.0f,  34.0f, 250.0f, 1.3f, 9.5f, 104.0f},
        {"c_young_sea", 12.0f,  34.0f,  40.0f, 0.0f, 9.5f,  0.0f},
        {"d_calm",      4.0f,  34.0f, 250.0f, 0.0f, 9.5f,  0.0f},
        // 报障配置（2026-11-23 Danis 截图）：面板风速被拖到 0.7（几乎无风），
        // 其余 = 选型场默认值。这个档位下海面只剩细节短波，用来复现「硬边」。
        {"e_light_air", 0.7f,  34.0f, 250.0f, 0.0f, 9.5f,  0.0f},
    };
    const float theta_toward = (app.deg_.sun_azim_deg - 180.0f) * kPi / 180.0f;
    struct Shot { const char* suffix; float phi; float R; float theta; };
    const Shot kShots[6] = {
        {"45deg_5m",      45.0f * kPi / 180.0f, 5.0f / std::sin(45.0f * kPi / 180.0f),
         theta_toward},
        {"45deg_40m",     45.0f * kPi / 180.0f, 40.0f / std::sin(45.0f * kPi / 180.0f),
         theta_toward},
        {"10deg_grazing", 10.0f * kPi / 180.0f, 18.0f, theta_toward},
        // 远景：涌（λ≈140 m）在这种尺度上才读得出"两套方向交叉"
        {"wide_250m",     25.0f * kPi / 180.0f, 250.0f, theta_toward},
        // Danis 报障机位：pos(4.68, 53.50, 1.99) ⇒ R≈53.74、phi≈84.6°（近俯视）
        {"topdown_53m",   84.6f * kPi / 180.0f, 53.74f, theta_toward},
        // Danis 报障机位（2026-11-23 截图 HUD 的 CAM pos）
        //   pos(53.0605698, 34.0492859, 40.6229286) → target(0,0,0)
        //   ⇒ R=75.0、phi=27.0°、theta=52.5625°（ViewConfig 的球坐标口径）
        {"report_cam_75m", 27.0f * kPi / 180.0f, 75.0f, 52.5625f * kPi / 180.0f},
    };

    for (const Preset& p : kPresets) {
        app.water_cfg_.SetWind(p.wind_ms, p.wind_dir_deg * kPi / 180.0f);
        app.water_cfg_.fetch_km = (p.fetch_km <= 0.0f)
                                      ? std::numeric_limits<float>::infinity()
                                      : p.fetch_km;
        app.water_cfg_.SetSwell(p.swell_hs, p.swell_tp, p.swell_dir_deg * kPi / 180.0f);
        app.InvalidateLabWater();
        for (const Shot& s : kShots) {
            app.view_ = jpov_viewer::DefaultView();
            app.view_.phi = s.phi;
            app.view_.theta = s.theta;
            app.view_.R = s.R;
            const std::string path = out_dir + "/oceanlab_" + p.name + "_" + s.suffix + ".png";
            app.RunOnce(input, winfo, path.c_str());
        }
        const jpov::water::WaterReadout& r = app.lab_readout_;
        LOG(INFO) << p.name << ": Hs_wind=" << r.hs_wind_m << " Hs_swell=" << r.hs_swell_m
                  << " Tp=" << r.period_s << " lam_p=" << r.lambda_peak_m
                  << " rms_slope=" << r.rms_slope;
    }
    LOG(INFO) << "ocean lab captured to " << out_dir;
    app.Finalize();
    return 0;
}

// 临时：复现 Danis 2026-11-24 15:00 的截图配置（硬边报障）。不进 PR。
int RunCaptureWaterRepro(const std::string& out_dir) {
    constexpr float kPi = 3.14159265358979323846f;
    JPOV::Config cfg;
    cfg.title = "JPOV — 水体硬边复现（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };
    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetShowScene(true);
    app.Init();
    InstallScene(app);
    app.SetOceanLab(true);
    // —— 截图里的配置 ——
    app.water_cfg_.SetWind(8.0f, 34.0f * kPi / 180.0f);
    app.water_cfg_.fetch_km = 0.394f;
    app.water_cfg_.duration_h = std::numeric_limits<float>::infinity();
    app.water_cfg_.SetSwell(4.92f, 4.3f, 86.0f * kPi / 180.0f);
    app.water_cfg_.depth_m = 3.5f;
    app.water_cfg_.level_y = 0.0f;
    app.deg_.sun_elev_deg = 9.0f;
    app.deg_.sun_azim_deg = 214.0f;
    app.deg_.turbidity = 2.0f;
    app.exposure_ = 0.6f;
    app.SetWaterWaveTime(0.0f);          // 冻结在 0 s
    if (const char* dbg = std::getenv("JPOV_WAVE_DEBUG")) {
        app.SetWaterWaveDebug(std::atoi(dbg));
    }
    if (const char* comp = std::getenv("JPOV_WAVE_COMPONENTS")) {
        app.SetWaterWaveComponents(static_cast<uint32_t>(std::strtoul(comp, nullptr, 0)));
    }
    if (const char* solo = std::getenv("JPOV_WAVE_SOLO")) {
        app.SetWaterWaveSolo(std::atoi(solo));
    }
    app.SetWaterSkin(std::getenv("JPOV_NO_SKIN") == nullptr);
    app.view_.phi = 30.5 * kPi / 180.0;  // CAM pos (8.18, 7.07, 9.12) -> 原点
    app.view_.theta = 0.732f;
    app.view_.R = 13.9f;
    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, (out_dir + "/water_repro.png").c_str());
    LOG(INFO) << "water repro captured to " << out_dir;
    app.Finalize();
    return 0;
}

int RunCaptureOceanReport(const std::string& out_dir, const std::string& config_path) {
    const float kPi = 3.14159265358979323846f;
    OceanConfig oc;
    if (!config_path.empty()) {
        if (!LoadOceanConfig(config_path, &oc)) {
            return 1;
        }
    }
    {
        jpov::WaterWaves dbg_waves;
        jpov::water::WaterReadout dbg_readout;
        jpov::water::BuildWater(oc.water, jpov::water::WaterQuality{}, &dbg_waves,
                                &dbg_readout);
        LOG(INFO) << "ocean report: wind=" << oc.water.WindSpeedMs()
                  << " m/s, Hs=" << dbg_readout.hs_total_m
                  << " m, Tp=" << dbg_readout.period_s
                  << " s, λp=" << dbg_readout.lambda_peak_m << " m";
    }

    JPOV::Config cfg;
    cfg.title = "JPOV — 海面报告（4 合 1，headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetShowScene(false);   // 纯海面：不画岩板/方块/人（只留天空 + 水）
    app.SetWaterScene(true);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = oc.sun_elev_deg;
    app.deg_.sun_azim_deg    = oc.sun_azim_deg;
    app.deg_.turbidity       = oc.turbidity;

    // 水体 = 一份 WaterConfig（2026-11-24：配置文件不再能拼波表/波幅倍率）
    app.SetWaterConfig(oc.water);
    app.SetWaterLevel(oc.water.level_y);
    app.SetWaterSkin(true);
    app.SetWaterMedium(true);
    app.SetWaterTimeSpeed(oc.time_speed);
    app.SetWaterWaves(true);
    app.SetWaterWaveTime(7.0f);   // 冻结时间：4 张图的波场相位一致，可直接对比
    app.SetWaterWaveDebug(oc.debug);
    app.exposure_ = oc.exposure;

    // 朝向太阳 = 相机在太阳方位的对面（theta = azim − 180°）。
    const float theta_toward = (oc.sun_azim_deg - 180.0f) * kPi / 180.0f;
    const float kGrazing = 10.0f * kPi / 180.0f;
    struct OceanShot {
        const char* name;
        float phi;
        float radius;
        float theta;
    };
    const OceanShot kShots[4] = {
        {"ocean_1_45deg_5m.png",       45.0f * kPi / 180.0f,
         5.0f / std::sin(45.0f * kPi / 180.0f), theta_toward},
        {"ocean_2_down_40m.png",       45.0f * kPi / 180.0f,
         40.0f / std::sin(45.0f * kPi / 180.0f), theta_toward},
        {"ocean_3_grazing_sun.png",    kGrazing, 18.0f, theta_toward},
        {"ocean_4_grazing_away.png",   kGrazing, 18.0f, theta_toward + kPi},
    };
    for (const OceanShot& shot : kShots) {
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = shot.phi;
        app.view_.theta = shot.theta;
        app.view_.R     = shot.radius;
        app.RunOnce(input, winfo, (out_dir + "/" + shot.name).c_str());
    }

    LOG(INFO) << "ocean report captured to " << out_dir;
    app.Finalize();
    return 0;
}

// 三色环境光 → 水体照明的 A/B：证明「三色 ambient 塌成纯色后确实进了水体」。
// 用法：jpov_skylight_viewer --capture-water-tricolor <out_dir>
//
// 出图（同机位、同光照、介质开，只切三色 ambient 开关）：
//   tricolor_off.png  单色 ambient（= AmbientColor()，旧行为）
//   tricolor_on.png   三色 ambient（天/天际线/地 按 1:2:1 塔成纯色后供介质使用）
//
// 判据：两图必须**不同**（旧实现里三色开关对水完全无效）；差别应集中在
//   水体被照亮的地方（岩板/方块周围的水而不是天空）。
int RunCaptureWaterTricolor(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 三色 ambient → 水体 A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 机位与水介质 A/B 的机位 A 一致（俯视 35°），岩板深度取默认可见档。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.62f;
    app.view_.theta = 0.6f;
    app.view_.R     = 26.0f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;
    app.SetSlabDepth(jpov_skylight::kSlabTopDepthDefM);
    app.SetWaterMedium(true);

    app.SetTricolorAmbient(false);
    app.RunOnce(input, winfo, (out_dir + "/tricolor_off.png").c_str());
    app.SetTricolorAmbient(true);
    app.RunOnce(input, winfo, (out_dir + "/tricolor_on.png").c_str());

    LOG(INFO) << "water tricolor A/B captured to " << out_dir
              << " (tricolor_off / tricolor_on)";
    app.Finalize();
    return 0;
}

// 水面上的「太阳的像」（解析 GGX，盘状光源）—— 三张不同太阳方位的 A/B。
// 用法：jpov_skylight_viewer --capture-water-sun <out_dir>
//
// 出图：sun_azim{34,214,219,224}.png —— 同机位、同介质，只转太阳方位。
//   34° 是**对照**：太阳在相机背后 ⇒ 日盘像落在画外，水面**不该**有亮斑；
//   214/219/224° 是相机朝向一侧 ⇒ 亮斑应随方位平移。
// 判据：水面上那个 **≈6 px 的亮斑**跟着太阳方位**横向移动**，且峰值远超环境反射
//       （峰值是 HDR 值，tone map 后应该是纯白）；没有它说明带宽/遮罩哪一步失效了。
int RunCaptureWaterSun(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水面太阳的像（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 俯视 35°，看得到大片水面 —— 日盘像要落在水体上才有意义。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.62f;
    app.view_.theta = 0.6f;
    app.view_.R     = 26.0f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 28.0f;   // 低太阳 ⇒ 掠射反射，日盘像离相机近、更好找
    app.SetSlabDepth(jpov_skylight::kSlabTopDepthDefM);
    app.SetWaterMedium(true);

    // ⚠️ 方位要取**相机自己的朝向上**才看得到镜像：水面把视线水平方向原样反射回去 ⇒
    //   日盘像只出现在「相机水平朝向 ≈ 太阳方位」的那一侧。本机位 theta=0.6 ⇒ 相机在
    //   方位 34.4°、朝 214.4° 看 ⇒ 太阳方位要取 214° 附近（之前取 25°/45° 时像在画外，
    //   白拍了一轮）。
    for (float azim : {34.0f, 214.0f, 219.0f, 224.0f}) {
        app.deg_.sun_azim_deg = azim;
        char name[64];
        std::snprintf(name, sizeof(name), "/sun_azim%03d.png", static_cast<int>(azim));
        app.RunOnce(input, winfo, (out_dir + name).c_str());
    }

    // 诊断用：关掉**水体介质**（保留水皮与太阳）—— 用来分清亮点是「水皮的镜面项」
    // 还是「水体体积的太阳内散射」。介质关掉后体积项消失；若亮点还在 ⇒ 它是水皮。
    {
        app.deg_.sun_azim_deg = 214.0f;
        app.SetWaterMedium(false);
        app.RunOnce(input, winfo, (out_dir + "/sun_medium_off.png").c_str());
        app.SetWaterMedium(true);
    }

    // 诊断用：保持 sky / ambient 不变，只把**太阳强度**置零。
    // 用来分辨「水面那个亮点」是太阳的解析镜面项（会消失），
    // 还是天空里绘出的日盘透过水面 / 水体自身的散射（不会消失）。
    {
        jpov::DirectionalLight no_sun{};
        no_sun.direction = {-0.5f, -0.7f, -0.5f};
        no_sun.intensity = 0.0f;
        app.SetSunOverride(no_sun);
        app.deg_.sun_azim_deg = 214.0f;
        app.RunOnce(input, winfo, (out_dir + "/sun_off.png").c_str());
        app.ClearSunOverride();
    }

    LOG(INFO) << "water sun-image captured to " << out_dir
              << " (34 对照 / 214 / 219 / 224 / sun_off / sun_medium_off)";
    app.Finalize();
    return 0;
}

// 用法：jpov_skylight_viewer --capture-mirror-specular <out_dir>
//
// **镜面高光的带宽（光源角半径 θ）** 的人类可读验收拍摄。
//
// 要验的一件事：`metallic=1, roughness=0` 的大镜面地面 + 平行光（太阳）⇒ 地面上必须
//   出现**太阳的像**（一个 ≈6 px 的亮盘，与天上那颗日盘同尺寸）。
//   完美镜面 + 平行光 = 两个 δ 相乘，逐像素采样只能拿到 ∞ 或 0 ⇒ 修之前整块地面是死的
//   （纯黑，一个比底色亮的像素都没有，见 docs/jpov_specular_bandwidth.md）；把太阳的角半径 θ 并进
//   GGX 的带宽（`a2 += (θ/2)²`）后 D 才有有限峰值 ⇒ 亮盘出现。
//
// 机位是**算死**的，不靠肉眼对齐：地面 y=0，相机 = (R cosφ sinθ, R sinφ, R cosφ cosθ)
//   看向原点；原点处的反射方向 = 仰角 φ、方位 θ+180。取 θ+180 = 太阳方位、φ = 太阳仰角
//   ⇒ 太阳的像精确落在画面正中（原点）。太阳本身也在画面上方 ⇒ 一张图里能直接比较
//   「天上的日盘」与「地上的像」的大小与亮度。
//
// 出图（同一机位/同一太阳，只切地面 roughness；全部是**正常观感**）：
//   mirror_r0.000.png   r=0 的完美镜面：地面映天空与方块，日盘的像落在正中
//   mirror_r0.020.png   θ 主导（a2 只多 3%）⇒ 应与 r=0 几乎同图
//   mirror_r0.300.png   粗糙端：roughness 主导，仍该有光
// ⚠️ 刻意**不**出"关掉天光与环境光、只剩太阳镜面项"的诊断照：那种图地面几乎全黑，
//    检查时容易被误读成"材质/渲染坏了"（Danis 2026-11-18 定）。
int RunCaptureMirrorSpecular(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 镜面高光带宽（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallMirrorFloorScene();   // 地面 + 散布方块（不装模型/蓝人，无需外部资产）
    app.SetMirrorSpecularView();     // 相机/太阳：太阳的像落在画面正中

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 天光消费量取 0.35（不是 1.0）：镜面在「太阳的像」那个方向映的正是日盘周围最亮
    // 的天光，全天光 + 暗银镜会把两者一起过曝成白，日盘的像就淹在白里了。
    // 0.35 既能看出"这是镜子（映天、映方块）"，又给日盘留出对比度。
    app.SetEnvSpecular(true, 0.35f);
    app.SetSsr(true);

    // ① 钱图（r=0 完美镜面）：地面映天空与散布方块，日盘的像叠在正中。
    //    低太阳掠过地面时 lobe 沿视线方向被投影拉长，是正确行为。
    app.SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(/*roughness*/ 0.0f));
    shoot("mirror_r0.000");

    // ② roughness=0.02（JPOV 镜面地板预设）：修之前被 `max(denom,1e-7)` 护栏钳成一块
    //    暗糊团（越光滑反而越暗）；现在与 r=0 几乎同一颗盘（θ 主导）。
    app.SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(/*roughness*/ 0.02f));
    shoot("mirror_r0.020");

    // ③ 粗糙端（roughness 主导）：仍该有光。
    app.SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(/*roughness*/ 0.30f));
    shoot("mirror_r0.300");

    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "mirror specular captured to " << out_dir;
    return 0;
}

// 用法：jpov_skylight_viewer --capture-point-specular <out_dir>
//
// **点光源的镜面带宽（逐像素角半径 θ = asin(R/dist)）** 的人类可读验收拍摄。
//
// 与 `--capture-mirror-specular`（平行光）同一个问题，只是 θ 逐像素、由灯泡尺寸决定：
//   · 修之前 `physical_radius` 只用来挪 `spec_dir`（代表点），**a2 一点没变** ⇒
//     r=0 的镜面地面上完全没有灯的高光；有光泽的地面上也只是一个「点」（与灯多大无关）。
//   · 接上 θ 后 a2 += (θ/2)² ⇒ 灯越大，地上那块「灯的像」越大越柔（能量守恒）。
//
// 机位与灯位是**算死**的（推导见 SkylightApp::PlacePointLampAtScreenCenter）：
//   灯放在「原点处的反射方向」上 3.5 m 处 ⇒ 灯的像精确落在画面正中。
//   另画一个自发光小球（直径 = 2R）当灯的参照物，一眼能对上「天上的灯 / 地上的像」。
//
// 出图（同一机位，只改灯的物理半径；全部是正常镜子观感 —— 太阳关掉、天光反射开着）：
//   point_r0.020.png   小球灯
//   point_r0.100.png   中等（≈人手一个灯笼）
//   point_r0.500.png   大球灯 ⇒ 像最大最柔
// ⚠️ 刻意**不**出"关掉天光、只剩这盏灯"的诊断照（地面几乎全黑，容易被误读成渲染坏了）。
int RunCapturePointSpecular(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 点光源镜面带宽（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallMirrorFloorScene();
    app.SetPointSpecularView();

    // 地面：纯镜面（roughness=0）。这样地面上的高光**只能**来自灯的角半径 ——
    // 如果带宽没接上，D ≡ 0，画面里就一块灯的像都没有。
    app.SetFloorMaterialOverride(jpov_skylight::MirrorFloorMaterial(/*roughness*/ 0.0f));

    // 太阳与环境光全关：否则太阳的高光（比灯亮几个量级）会盖住要看的东西。
    jpov::DirectionalLight no_sun{};
    no_sun.direction = {0.0f, -1.0f, 0.0f};
    no_sun.intensity = 0.0f;
    app.SetSunOverride(no_sun);

    app.SetEnvSpecular(true, 0.35f);
    app.SetSsr(true);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // ① 灯越来越大 ⇒ 地上的像越来越大越柔（带宽 = (θ/2)²，θ = asin(R/3.5m)）。
    struct LampShot {
        float radius;
        const char* name;
    };
    const LampShot lamp_shots[] = {
        {0.02f, "point_r0.020"},
        {0.10f, "point_r0.100"},
        {0.50f, "point_r0.500"},
    };
    for (const LampShot& s : lamp_shots) {
        app.PlacePointLampAtScreenCenter(s.radius);
        shoot(s.name);
    }

    app.ClearPointLamp();
    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "point specular captured to " << out_dir;
    return 0;
}

// 「水体消费的是光，不是 sky」的验证拍摄。
// 用法：jpov_skylight_viewer --capture-water-light <out_dir>
//
// 三张图**sky 完全相同**，只改 RenderCommandList 的光源：
//   light_sky.png    光源 = 由 sky 推导的默认 ambient/sun（= 改动前也能出的图）
//   light_warm.png   ambient 覆盖成暖橙、sun 不变（sky 不动）
//                  ⇒ 水体/物体应变暖；旧实现里水只认 sky，这张图不可能变
//   light_dark.png   ambient = 0 且 sun intensity = 0
//                  ⇒ 注入为 0，水体只剩吸收（背景被吞、“没光就暗”）
int RunCaptureWaterLight(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水体照明来源 A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 与水介质 A/B 的机位 A 一致：俯视 35°，能看到岩板、三件方块与它们周围的水体。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.62f;
    app.view_.theta = 0.6f;
    app.view_.R     = 26.0f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;
    app.SetSlabDepth(jpov_skylight::kSlabTopDepthDefM);
    app.SetWaterMedium(true);

    app.RunOnce(input, winfo, (out_dir + "/light_sky.png").c_str());

    // 只关太阳强度（sky 不动、ambient 不动）：用来分辨「水面上的亮点」到底来自
    // 太阳的解析项，还是天空里绘出的日盘透过水面（透射）。
    // 前者会消失，后者会留下。
    {
        jpov::DirectionalLight no_sun{};
        no_sun.direction = {-0.5f, -0.7f, -0.5f};
        no_sun.intensity = 0.0f;
        app.SetSunOverride(no_sun);
        app.RunOnce(input, winfo, (out_dir + "/light_sunoff.png").c_str());
        app.ClearSunOverride();
    }

    // 只换 ambient（sky / sun 不动）：暖橙、强度略高，便于测像素。
    jpov::AmbientLight warm;
    warm.color = {0.90f, 0.35f, 0.12f, 1.0f};
    warm.intensity = 1.6f;
    app.SetAmbientOverride(warm);
    app.RunOnce(input, winfo, (out_dir + "/light_warm.png").c_str());
    app.ClearAmbientOverride();

    // 完全无光：ambient = 0、sun 强度 = 0。水只剩吸收，注入应为零。
    jpov::AmbientLight none_ambient;
    none_ambient.color = {0.0f, 0.0f, 0.0f, 1.0f};
    none_ambient.intensity = 0.0f;
    app.SetAmbientOverride(none_ambient);
    jpov::DirectionalLight none_sun{};
    none_sun.intensity = 0.0f;
    app.SetSunOverride(none_sun);
    app.RunOnce(input, winfo, (out_dir + "/light_dark.png").c_str());

    LOG(INFO) << "water lighting A/B captured to " << out_dir
              << " (light_sky / light_sunoff / light_warm / light_dark)";
    app.Finalize();
    return 0;
}

// 水体连续性扫描（诊断用）：固定相机/光照，沿岩板深度做细步扫描。
// 用法：jpov_skylight_viewer --capture-water-sweep <out_dir>
// 出图：sweep_d000.png … sweep_d600.png（0.00 ~ 6.00 m，步长 0.25）
//
// 为什么需要：掠射角下物体“入水/出水”的过渡是不是**连续**的，胉眼看容易被帧率+
// 屏幕位移骗过去（红通道半衰 ~0.2 m，而屏幕只动十几像素）。把中间帧存下来逐帧量像素，
// 就能区分“物理上本来就陡”与“积分真断了”。
int RunCaptureWaterSweep(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水体连续性扫描（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 用户报告问题时的机位（CAM pos (−60.4857, 4.1991, −41.6154) -> (0,0,0)）：
    // R = 73.54、仰角 3.27°、方位 4.1095 rad。掠射 5.8°，正好是“看着像瞬间”的角度。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.R     = 73.54f;
    app.view_.phi   = 0.05715f;
    app.view_.theta = 4.1095f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;
    app.SetWaterMedium(true);

    for (int i = 0; i <= 24; ++i) {
        const float depth = 0.25f * static_cast<float>(i);
        app.SetSlabDepth(depth);
        char name[64];
        std::snprintf(name, sizeof(name), "%s/sweep_d%03d.png", out_dir.c_str(),
                      static_cast<int>(depth * 100.0f + 0.5f));
        app.RunOnce(input, winfo, name);
    }

    LOG(INFO) << "water continuity sweep captured to " << out_dir
              << " (0.00 .. 6.00 m / step 0.25)";
    app.Finalize();
    return 0;
}

// 相机跨越水面的连续性扫描（诊断用）：相机高度从水上 → 水面 → 水下，其他参数不变。
// 用法：jpov_skylight_viewer --capture-camera-cross <out_dir>
// 出图：cross_p+100.png … cross_p-100.png（view_.phi = +0.10 … −0.10 rad，步长 0.01）
//
// 为什么单独一个：岩板深度扫描检的是“物体入水”，这里检的是**介质分支本身**
// 在 ro.y = level 两侧是不是连续的（水上要走“先穿水面”，水下从 t=0 开始）。
int RunCaptureCameraCross(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 相机跨越水面（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 固定 theta / R，只动 phi：R=20 ⇒ phi=0.10 时相机高度 +2.00 m，−0.10 时 −2.00 m。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.R     = 20.0f;
    app.view_.theta = 0.6f;
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;
    app.SetWaterMedium(true);
    app.SetSlabDepth(5.0f);

    for (int i = 10; i >= -10; --i) {
        app.view_.phi = 0.01f * static_cast<float>(i);
        char name[64];
        std::snprintf(name, sizeof(name), "%s/cross_p%+03d.png", out_dir.c_str(), i);
        app.RunOnce(input, winfo, name);
    }

    LOG(INFO) << "camera-cross captured to " << out_dir
              << " (phi +0.10 .. −0.10 rad, step 0.01)";
    app.Finalize();
    return 0;
}

// 天球折射鉴定（水下抬头看天）：同一机位/光照，只切折射开关。
// 用法：jpov_skylight_viewer --capture-sky-refract <out_dir>
// 出图：refract_off.png / refract_on.png
//
// 机位在水下 4.5 m（R=8、phi=−0.6），视线向上 34° —— 正好把 Snell 窗（半角 48.6°）
// 与太阳（气侧 45° → 水内应出现在 58° 仰角）都装进画面。
int RunCaptureSkyRefract(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 天球折射鉴定（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetWaterScene(true);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);
    app.BuildWaterPeople();

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.view_ = jpov_viewer::DefaultView();
    app.view_.R     = 8.0f;
    app.view_.phi   = -0.87f;   // 相机在水下 ≈6.1 m，视线向上 ≈50°（窗与太阳都在画面内）
    app.view_.theta = 0.70f + 3.14159265f;   // 相机在太阳的对侧看回去（否则太阳在身后）
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;
    app.SetWaterMedium(true);
    app.SetSlabDepth(5.0f);

    // 没有独立开关：折射由「有水面 + 相机在水下」自动开，折射率取水配置。
    // ior = 1.0 ⇒ 折射恒等、n² = 1 ⇒ 等价于“不折射”，作为 A/B 的基线。
    app.SetWaterIor(1.0f);
    app.RunOnce(input, winfo, (out_dir + "/refract_off.png").c_str());
    app.SetWaterIor(jpov_skylight::kWaterIor);
    app.RunOnce(input, winfo, (out_dir + "/refract_on.png").c_str());

    LOG(INFO) << "sky refraction A/B captured to " << out_dir;
    app.Finalize();
    return 0;
}

// SSR A/B 拍摄：固定机位/光照，同一个"地表大镜子 + 上方随机模型"场景，出四张图：
//   ssr_off.png        (a) 只有环境镜面（今天的样子）——SSR 不能比它差
//   ssr_on_1x.png      (b) SSR 全分辨率 1:1 ——**质量基准**
//   ssr_on_half.png    (c) SSR 半分辨率 1:2 + A 预乘双边升采样 —— 本次方案
//   ssr_coverage.png   (d) A 覆盖率灰度（看洞/漏在哪）
// 用法：jpov_skylight_viewer --capture-ssr <out_dir>
// 地面材质阶梯（2026-11-02）：SSR 粗糙度那条链的验收拍摄。
//
// 为什么要这组图：roughness 的正确性**只能靠材质对比**判断 ——
//   镜面要锐利、金属要糊但仍是有形状的反射、石砖近处几乎没有倒影而远处掠射发亮、
//   岩石基本只剩一层淡淡的天光。单看一张图无法判断"这糊得对不对"。
//
// 出图（全部同一机位/光照，只在坐标或材质上不同）：
//   floor0_mirror.png  镜面（metal=1, r=0.02）—— 锐利倒影的基准
//   floor1_metal.png   金属（metal=1, r=0.30）—— 磨砂的基准
//   floor2_stone.png   光滑石砖（metal=0, r=0.20）
//   floor3_rock.png    粗糙岩石（metal=0, r=0.85）—— 应几乎只看到漫反射
//   ladder_ssr_on.png  四段拼接（近→远：镜面/金属/石砖/岩石）
//   ladder_ssr_off.png 同场景、关 SSR —— 纯 env 的对照（"单调改进"的基线）
int RunCaptureMaterials(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — SSR 地面材质阶梯（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.scene_index_ = 1;
    app.SetEnvSpecular(true, 1.0f);
    app.deg_.turbidity = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 35.0f;
    app.deg_.sun_azim_deg = 60.0f;
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.21;   // ≈12°：掠射最能看出菲涅尔与模糊量的差别
    app.view_.theta = 0.15;
    app.view_.R     = 45.0;
    app.ssr_scale_ = 0.5f;
    app.ssr_debug_ = 0;

    // 单材质：四个档位各一张（SSR 开）。
    app.SetSsr(true);
    const char* names[4] = {"floor0_mirror", "floor1_metal", "floor2_stone", "floor3_rock"};
    for (int i = 0; i < 4; ++i) {
        app.floor_preset_ = i;
        app.RunOnce(input, winfo, (out_dir + "/" + names[i] + ".png").c_str());
        LOG(INFO) << "capture: " << out_dir << "/" << names[i] << ".png";
    }

    // 模糊量的 A/B：同一块金属（r=0.30）在"磨砂强度=0"（等价旧行为，永远锐利）
    // 与"磨砂强度=8"（本次新行为）下的对照图 —— 这是"逐渐磨砂"是否真的生效的直接证据。
    app.floor_preset_ = 1;
    const float saved_lod_slope = app.ssr_rough_to_lod_;
    app.ssr_rough_to_lod_ = 0.0f;
    app.RunOnce(input, winfo, (out_dir + "/metal_lod0_sharp.png").c_str());
    app.ssr_rough_to_lod_ = saved_lod_slope;

    // 镜面 + SMAA：回答"最终帧的 SMAA 能不能救倒影的锯齿"（因为倒影是在 1:2 分辨率上
    // 追踪、再双边升采样回全分辨率，锯齿的步长是 2 屏幕像素）。
    app.floor_preset_ = 0;
    app.SetSmaa(true, 0.1f);
    app.RunOnce(input, winfo, (out_dir + "/mirror_smaa_on.png").c_str());
    app.SetSmaa(false, 0.1f);

    // 四段拼接：开 / 关 SSR 两张（关 SSR = 纯 env 基线）。
    // 相机拉远一点：四段沿 X 并排（±40 m），要让四条都完整入画。
    app.view_.phi   = 0.26;
    app.view_.theta = 0.0;
    app.view_.R     = 52.0;
    app.floor_preset_ = jpov_skylight::kFloorLadder;
    app.SetSsr(true);
    app.RunOnce(input, winfo, (out_dir + "/ladder_ssr_on.png").c_str());
    app.SetSsr(false);
    app.RunOnce(input, winfo, (out_dir + "/ladder_ssr_off.png").c_str());
    // 覆盖率（调试档 1）：看哪些像素真的发了射线并命中（粗糙段应当整段为黑 = 已截止）。
    app.SetSsr(true);
    app.ssr_debug_ = 1;
    app.RunOnce(input, winfo, (out_dir + "/ladder_coverage.png").c_str());
    app.ssr_debug_ = 0;

    LOG(INFO) << "SSR 材质阶梯 captured to " << out_dir;
    app.Finalize();
    return 0;
}

int RunCaptureSsr(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — SSR A/B（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 场景 1 = 镜面地板（200×200 m）+ 24 个随机方块 + 桌子/橡树。
    app.scene_index_ = 1;
    app.SetEnvSpecular(true, 1.0f);
    app.deg_.turbidity = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 35.0f;
    app.deg_.sun_azim_deg = 60.0f;
    // 低角度相机：掠射最能暴露自交/厚度/漏光。目标点固定在原点（镜面中心）。
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.21;   // ≈12°
    app.view_.theta = 0.6;
    app.view_.R     = 45.0;
    // SSR_CAM="px,py,pz"：按 HUD 的相机位置反解 phi/theta/R（目标恒为原点）。
    if (std::getenv("SSR_CAM") != nullptr) {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        std::sscanf(std::getenv("SSR_CAM"), "%f,%f,%f", &px, &py, &pz);
        const double R = std::sqrt(double(px) * px + double(py) * py + double(pz) * pz);
        app.view_.R = R;
        app.view_.phi = std::asin(py / R);
        app.view_.theta = std::atan2(px, pz);
    }

    // (a) 关 SSR
    app.SetSsr(false);
    app.RunOnce(input, winfo, (out_dir + "/ssr_off.png").c_str());

    // (b) 1:1（质量基准）
    app.SetSsr(true);
    app.ssr_scale_ = 1.0f;
    app.ssr_debug_ = 0;
    app.RunOnce(input, winfo, (out_dir + "/ssr_on_1x.png").c_str());

    // (b3) 掠射淡出 A/B（2026-11-13）
    if (std::getenv("SSR_GRAZING") != nullptr) {
        if (const char* v = std::getenv("SSR_GRAZING_START")) {
            app.ssr_grazing_start_ = std::strtof(v, nullptr);
        }
        if (const char* v = std::getenv("SSR_GRAZING_END")) {
            app.ssr_grazing_end_ = std::strtof(v, nullptr);
        }
        app.RunOnce(input, winfo, (out_dir + "/ssr_grazing_on_1x.png").c_str());
        app.ssr_grazing_start_ = 1.0e9f;   // 关闭 = end <= start
        app.ssr_grazing_end_ = 1.0e9f;
        app.RunOnce(input, winfo, (out_dir + "/ssr_grazing_off_1x.png").c_str());
    }

    // (b7) A 的三个淡出（2026-11-13）：SSR_FADE=1 ⇒ 五张图。
    //   全关 / 全开 / 只预算 / 只 P 贴边 / 只 Q 贴边。
    //   带宽单位是**全分辨率屏幕像素**（面板同口径）；三个 env 可单独覆盖。
    if (std::getenv("SSR_FADE") != nullptr) {
        float budget = 60.0f;
        float edge_p = 40.0f;
        float edge_q = 40.0f;
        if (const char* v = std::getenv("SSR_FADE_BUDGET")) {
            budget = std::strtof(v, nullptr);
        }
        if (const char* v = std::getenv("SSR_FADE_EDGE")) {
            edge_p = std::strtof(v, nullptr);
        }
        if (const char* v = std::getenv("SSR_FADE_HIT_EDGE")) {
            edge_q = std::strtof(v, nullptr);
        }
        app.ssr_fade_budget_px_ = 0.0f;
        app.ssr_fade_edge_px_ = 0.0f;
        app.ssr_fade_hit_edge_px_ = 0.0f;
        app.RunOnce(input, winfo, (out_dir + "/ssr_fade_off_1x.png").c_str());

        app.ssr_fade_budget_px_ = budget;
        app.ssr_fade_edge_px_ = edge_p;
        app.ssr_fade_hit_edge_px_ = edge_q;
        app.RunOnce(input, winfo, (out_dir + "/ssr_fade_on_1x.png").c_str());

        app.ssr_fade_edge_px_ = 0.0f;
        app.ssr_fade_hit_edge_px_ = 0.0f;
        app.RunOnce(input, winfo, (out_dir + "/ssr_fade_budget_1x.png").c_str());

        app.ssr_fade_budget_px_ = 0.0f;
        app.ssr_fade_edge_px_ = edge_p;
        app.RunOnce(input, winfo, (out_dir + "/ssr_fade_edgep_1x.png").c_str());

        app.ssr_fade_edge_px_ = 0.0f;
        app.ssr_fade_hit_edge_px_ = edge_q;
        app.RunOnce(input, winfo, (out_dir + "/ssr_fade_edgeq_1x.png").c_str());

        app.ssr_fade_budget_px_ = 0.0f;
        app.ssr_fade_edge_px_ = 0.0f;
        app.ssr_fade_hit_edge_px_ = 0.0f;
    }

    // (b8) 遮丑滤波器（2026-11-13）：SSR_FILTER=1 ⇒ 五张图，**逐条独立**对比。
    //   off / hole（补洞）/ clamp（离群）/ atrous（小波）/ all（三条全开）。
    //   SSR_FILTER_ITERS 改小波趟数（默认 2）；SSR_FILTER_K 改离群抑制的 k·σ（默认 2）。
    if (std::getenv("SSR_FILTER") != nullptr) {
        if (const char* v = std::getenv("SSR_FILTER_ITERS")) {
            app.ssr_filter_atrous_iters_ = std::strtof(v, nullptr);
        }
        if (const char* v = std::getenv("SSR_FILTER_K")) {
            app.ssr_filter_clamp_k_ = std::strtof(v, nullptr);
        }
        app.ssr_filter_hole_ = false;
        app.ssr_filter_clamp_ = false;
        app.ssr_filter_atrous_ = false;
        app.RunOnce(input, winfo, (out_dir + "/ssr_filter_off_1x.png").c_str());

        app.ssr_filter_hole_ = true;
        app.RunOnce(input, winfo, (out_dir + "/ssr_filter_hole_1x.png").c_str());
        app.ssr_filter_hole_ = false;

        app.ssr_filter_clamp_ = true;
        app.RunOnce(input, winfo, (out_dir + "/ssr_filter_clamp_1x.png").c_str());
        app.ssr_filter_clamp_ = false;

        app.ssr_filter_atrous_ = true;
        app.RunOnce(input, winfo, (out_dir + "/ssr_filter_atrous_1x.png").c_str());

        app.ssr_filter_hole_ = true;
        app.ssr_filter_clamp_ = true;
        app.RunOnce(input, winfo, (out_dir + "/ssr_filter_all_1x.png").c_str());

        app.ssr_filter_hole_ = false;
        app.ssr_filter_clamp_ = false;
        app.ssr_filter_atrous_ = false;
    }

    // (b2) 柱体已经是"三源取最厚"（无开关），不再产出 slab0/slab1 对照图。

    // (b6) 背面命中：两个正交 flag 的 A/B（2026-11-13）
    //   SSR_BACKFACE=1 ⇒ 出四张：掩码 / 交回 env 且压暗 env / 用 SSR 颜色且压暗 / 用 SSR 颜色不压暗。
    //   SSR_BACKFACE_DIM 可改压暗比（默认 0.25）。
    if (std::getenv("SSR_BACKFACE") != nullptr) {
        float dim = 0.25f;
        if (const char* v = std::getenv("SSR_BACKFACE_DIM")) {
            dim = std::strtof(v, nullptr);
        }
        app.ssr_backface_use_ssr_ = false;
        app.ssr_backface_dim_ = 1.0f;
        app.ssr_debug_ = 5;   // 背面命中掩码（先看范围）
        app.RunOnce(input, winfo, (out_dir + "/ssr_backface_mask.png").c_str());
        app.ssr_debug_ = 0;
        app.RunOnce(input, winfo, (out_dir + "/ssr_backface_off.png").c_str());

        app.ssr_backface_use_ssr_ = false;      // 用 env，且把 env 压到 dim
        app.ssr_backface_dim_ = dim;
        app.RunOnce(input, winfo, (out_dir + "/ssr_backface_envdim.png").c_str());

        app.ssr_backface_use_ssr_ = true;       // 用 SSR 颜色 × dim
        app.RunOnce(input, winfo, (out_dir + "/ssr_backface_ssr.png").c_str());

        app.ssr_backface_dim_ = 1.0f;           // 用 SSR 颜色但不压暗
        app.RunOnce(input, winfo, (out_dir + "/ssr_backface_ssr_full.png").c_str());

        app.ssr_backface_use_ssr_ = false;      // 回默认
        app.ssr_backface_dim_ = 1.0f;
    }

    // (c) 1:2 + 升采样
    app.ssr_scale_ = 0.5f;
    app.RunOnce(input, winfo, (out_dir + "/ssr_on_half.png").c_str());

    // (d) A 覆盖率（1:2）
    app.ssr_debug_ = 1;
    app.RunOnce(input, winfo, (out_dir + "/ssr_coverage.png").c_str());

    // (d2) ray_ratio 视图（预算占用率）：验证新输出通道的口径
    app.ssr_debug_ = 4;
    app.RunOnce(input, winfo, (out_dir + "/ssr_ratio.png").c_str());
    app.ssr_debug_ = 0;

    // (d3) 1:3（N=3 不是 2 的幂，验证这条通路也成立）
    app.ssr_debug_ = 0;
    app.ssr_scale_ = 1.0f / 3.0f;
    app.RunOnce(input, winfo, (out_dir + "/ssr_on_third.png").c_str());
    app.ssr_scale_ = 0.5f;

    // (e) 升采样的深度判据只有一套（联合软权，2026-11-02 删掉 Unity 对照档）。
    app.ssr_debug_ = 0;
    app.RunOnce(input, winfo, (out_dir + "/ssr_half.png").c_str());

    // (f) **掠射**机位（R=45 m、离地 0.042 m）：横向条纹就是在这里暴露的。
    app.view_.R = 45.0;
    app.view_.phi = std::asin(0.042 / 45.0);
    app.view_.theta = 0.0;
    app.RunOnce(input, winfo, (out_dir + "/ssr_graze.png").c_str());


    LOG(INFO) << "SSR A/B captured to " << out_dir;
    app.Finalize();
    return 0;
}

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
    // 用法：jpov_skylight_viewer --capture /tmp/sky_out
    std::string out_dir;
    std::string smaa_out_dir;
    std::string env_out_dir;
    std::string ssr_out_dir;
    std::string mat_out_dir;
    std::string water_out_dir;
    std::string water_waves_out_dir;
    bool start_water_waves = false;   // --water-waves：直接进水场景并打开波浪
    float wave_time_s = -1.0f;        // --wave-time：冻结到这个秒数（<0 = 不冻结）
    std::string horizon_repro_out_dir;
    std::string water_coverage_out_dir;
    std::string box_cut_out_dir;
    std::string water_sweep_out_dir;
    std::string water_tricolor_out_dir;
    std::string water_light_out_dir;
    std::string water_sun_out_dir;
    std::string mirror_spec_out_dir;
    std::string point_spec_out_dir;
    std::string camera_cross_out_dir;
    std::string sky_refract_out_dir;
    std::string ocean_lab_out_dir;
    std::string ocean_lab_ui_out_dir;
    bool start_ocean_lab = false;   // --ocean-lab：直接进海面选型场（交互）
    std::string water_repro_out_dir;   // 临时：水体硬边复现
    std::string ocean_report_out_dir;
    std::string ocean_config_path;
    float ocean_time_s = 7.0f;   // 海面选型场拍摄用哪个时刻（秒；冻结）
    int water_debug = 0;         // 水皮 debug 档（1=法线 2=法线方差 11=探针 LOD）
    float smaa_threshold = 0.1f;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-smaa") == 0 && i + 1 < argc) {
            smaa_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-env") == 0 && i + 1 < argc) {
            env_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-ssr") == 0 && i + 1 < argc) {
            ssr_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-materials") == 0 && i + 1 < argc) {
            mat_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water") == 0 && i + 1 < argc) {
            water_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-waves") == 0 && i + 1 < argc) {
            water_waves_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--water-waves") == 0) {
            start_water_waves = true;
        } else if (std::strcmp(argv[i], "--wave-time") == 0 && i + 1 < argc) {
            wave_time_s = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--capture-horizon-repro") == 0 && i + 1 < argc) {
            horizon_repro_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-coverage") == 0 && i + 1 < argc) {
            water_coverage_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-box-cut") == 0 && i + 1 < argc) {
            box_cut_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-sweep") == 0 && i + 1 < argc) {
            water_sweep_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-tricolor") == 0 && i + 1 < argc) {
            water_tricolor_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-light") == 0 && i + 1 < argc) {
            water_light_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-sun") == 0 && i + 1 < argc) {
            water_sun_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-mirror-specular") == 0 && i + 1 < argc) {
            mirror_spec_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-point-specular") == 0 && i + 1 < argc) {
            point_spec_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-camera-cross") == 0 && i + 1 < argc) {
            camera_cross_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-sky-refract") == 0 && i + 1 < argc) {
            sky_refract_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water-repro") == 0 && i + 1 < argc) {
            water_repro_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-ocean-report") == 0 && i + 1 < argc) {
            ocean_report_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ocean-lab") == 0) {
            start_ocean_lab = true;
        } else if (std::strcmp(argv[i], "--capture-ocean-lab") == 0 && i + 1 < argc) {
            ocean_lab_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-ocean-lab-ui") == 0 && i + 1 < argc) {
            ocean_lab_ui_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ocean-config") == 0 && i + 1 < argc) {
            ocean_config_path = argv[i + 1];
        } else if (std::strcmp(argv[i], "--ocean-time") == 0 && i + 1 < argc) {
            ocean_time_s = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--water-debug") == 0 && i + 1 < argc) {
            water_debug = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }
    if (!env_out_dir.empty()) {
        return RunCaptureEnv(env_out_dir);
    }
    if (!ocean_lab_ui_out_dir.empty()) {
        return RunCaptureOceanLab(ocean_lab_ui_out_dir, /*show_ui=*/true, ocean_time_s,
                                  water_debug);
    }
    if (!ocean_lab_out_dir.empty()) {
        return RunCaptureOceanLab(ocean_lab_out_dir, /*show_ui=*/false, ocean_time_s,
                                  water_debug);
    }
    if (!water_repro_out_dir.empty()) {
        return RunCaptureWaterRepro(water_repro_out_dir);
    }
    if (!ocean_report_out_dir.empty()) {
        return RunCaptureOceanReport(ocean_report_out_dir, ocean_config_path);
    }
    if (!box_cut_out_dir.empty()) {
        return RunCaptureBoxCut(box_cut_out_dir);
    }
    if (!water_coverage_out_dir.empty()) {
        return RunCaptureWaterCoverage(water_coverage_out_dir);
    }
    if (!horizon_repro_out_dir.empty()) {
        return RunCaptureHorizonRepro(horizon_repro_out_dir);
    }
    if (!water_waves_out_dir.empty()) {
        return RunCaptureWaterWaves(water_waves_out_dir);
    }
    if (!water_out_dir.empty()) {
        return RunCaptureWater(water_out_dir);
    }
    if (!water_sweep_out_dir.empty()) {
        return RunCaptureWaterSweep(water_sweep_out_dir);
    }
    if (!water_tricolor_out_dir.empty()) {
        return RunCaptureWaterTricolor(water_tricolor_out_dir);
    }
    if (!water_light_out_dir.empty()) {
        return RunCaptureWaterLight(water_light_out_dir);
    }
    if (!water_sun_out_dir.empty()) {
        return RunCaptureWaterSun(water_sun_out_dir);
    }
    if (!mirror_spec_out_dir.empty()) {
        return RunCaptureMirrorSpecular(mirror_spec_out_dir);
    }
    if (!point_spec_out_dir.empty()) {
        return RunCapturePointSpecular(point_spec_out_dir);
    }
    if (!camera_cross_out_dir.empty()) {
        return RunCaptureCameraCross(camera_cross_out_dir);
    }
    if (!sky_refract_out_dir.empty()) {
        return RunCaptureSkyRefract(sky_refract_out_dir);
    }
    if (!mat_out_dir.empty()) {
        return RunCaptureMaterials(mat_out_dir);
    }
    if (!ssr_out_dir.empty()) {
        return RunCaptureSsr(ssr_out_dir);
    }
    if (!smaa_out_dir.empty()) {
        return RunCaptureSmaa(smaa_out_dir, smaa_threshold);
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
    // 交互模式声明「想要硬件光栅」：这是个看效果的工具，落到软件光栅（llvmpipe /
    // WARP）上只能看幻灯片。JPOV 不能选设备，所以这里能做的只有两件事 ——
    // Windows 导出表里请求高性能 GPU（见 jpov.cc 顶部注释）+ Init 后核对并告警。
    // headless 拍摄**不设**（保持 kAny）：CI / 黄金图本来就跑在 llvmpipe 上，
    // 每次拍摄都刷一条「偏好落空」告警没有意义。
    jpov_viewer::PreferGpuForInteractiveViewer(cfg);
    // 显式声明字体：CJK 显中文滑条标签，Latin 做拉丁回退（路径相对 exe 的 fonts/，
    // build_jpov_skylight_viewer.sh 同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(true);
    app.Init();                       // 内部：GL context + 全部 shader + 字体
    LOG(INFO) << "[stage] Init 完成";
    app.InstallTextMeasure();

    // ── 场景静态资源（只建一次，不在 OneIteration 里重复构造/上传）──
    // 每个阶段前后各打一行：真卡上崩在资源加载/首帧时，日志最后一行就是崩在哪一步。
    LOG(INFO) << "[stage] InstallScene 开始";
    InstallScene(app);
    LOG(INFO) << "[stage] InstallScene 完成";
    LOG(INFO) << "[stage] InstallModels 开始（加载 glb + 贴图）";
    InstallModels(app);
    LOG(INFO) << "[stage] InstallModels 完成";
    LOG(INFO) << "[stage] InstallPerson 开始（骨骼/实例）";
    InstallPerson(app);
    LOG(INFO) << "[stage] InstallPerson 完成";

    // --water-waves：直接进水体场景 + 打开波浪（省掉下拉/勾选两步，验收用）。
    if (start_ocean_lab) {
        app.SetOceanLab(true);
    }
    if (start_water_waves) {
        app.SetWaterScene(true);
        app.BuildWaterPeople();
        app.SetWaterWaves(true);
    }
    if (wave_time_s >= 0.0f) {
        app.SetWaterWaveTime(wave_time_s);
    }

    // ── 初始视角：三方块斜前方，R=6 使三块（总宽 ~4.2m）完整入画。──
    // 海面选型场自己已经摆好机位（SetOceanLab→SetOceanLabView）：不要被这里的
    // 通用默认机位覆盖（否则 --ocean-lab 进去看到的是 phi=0.25/theta=45°/R=9.5，
    // 与场景 4 下拉里的那台机位不一致）。
    if (!start_ocean_lab) {
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = 0.25;                       // 略俯视（~14°）
        app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
        app.view_.R     = 9.5;
    }
    if (start_water_waves) {
        // 贴近水面、低空掠过：能同时看近处的波纹明暗与远处的碎光。
        app.view_.phi   = 0.22;
        app.view_.theta = 0.6;
        app.view_.R     = 18.0;
    }

    // 交互事件循环（阻塞）。
    LOG(INFO) << "[stage] 进入事件循环 Run()";
    app.Run();

    app.Finalize();
    return 0;
}

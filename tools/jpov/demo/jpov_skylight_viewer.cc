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


    // (i)(j) 远景仰角雾 × SSR：雾用 **MRT#1 场景深度**，而 SSR 合成是往同一个 FBO 的颜色
    //   附件上画的 —— 这一组用来确认 SSR 不会破坏 MRT#1（雾在开 SSR / 关 SSR 下必须一致）。
    app.deg_.elev_fog_on = true;
    app.SetSsr(false);
    app.RunOnce(input, winfo, (out_dir + "/ssr_fog_off.png").c_str());
    app.SetSsr(true);
    app.RunOnce(input, winfo, (out_dir + "/ssr_fog_on.png").c_str());
    app.deg_.elev_fog_on = false;
    app.SetSsr(false);

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
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }
    if (!env_out_dir.empty()) {
        return RunCaptureEnv(env_out_dir);
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

    // ── 初始视角：三方块斜前方，R=6 使三块（总宽 ~4.2m）完整入画。──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;                       // 略俯视（~14°）
    app.view_.theta = 3.14159265358979323846 / 4.0;  // 45° 方位
    app.view_.R     = 9.5;

    // 交互事件循环（阻塞）。
    LOG(INFO) << "[stage] 进入事件循环 Run()";
    app.Run();

    app.Finalize();
    return 0;
}

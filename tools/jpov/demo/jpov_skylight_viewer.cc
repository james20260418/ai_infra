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
// SSR 预设（--capture-ssr <out_dir>）：固定机位出 5 张 —— 关 / Hi-Z(2,32) /
//   Hi-Z(3,16) / 关 Hi-Z(0,64) / 1/4 分辨率，供肉眼对比参数平衡。
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
    app.mat_low_    = jpov_skylight::MaterialLowGloss();
    app.mat_high_   = jpov_skylight::MaterialHighGloss();
    app.mat_metal_  = jpov_skylight::MaterialMetal();
    app.mat_ground_ = jpov_skylight::GroundMaterial();
    // 镜面地板场景的材质：地板 = 金属 + 极低粗糙度（几乎完美镜面）；
    // 三个方块用高饱和纯色，倒影里一眼能认出"是谁的倒影"。
    app.mat_mirror_ = jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.62f, 0.64f, 0.66f, 1.0f}, /*metallic*/ 1.0f, /*roughness*/ 0.05f);
    app.mat_red_ = jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.72f, 0.25f, 0.22f, 1.0f}, /*metallic*/ 0.0f, /*roughness*/ 0.55f);
    app.mat_green_ = jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.22f, 0.55f, 0.30f, 1.0f}, /*metallic*/ 0.0f, /*roughness*/ 0.50f);
    app.mat_blue_ = jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.25f, 0.38f, 0.75f, 1.0f}, /*metallic*/ 0.0f, /*roughness*/ 0.45f);
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
    app.SetShowPanel(false);
    app.SetShowHud(false);   // 交付/验收图要干净；HUD 只给交互窗口截图用    // 纯 3D 截图（无面板）
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
    app.SetShowHud(false);   // 交付/验收图要干净；HUD 只给交互窗口截图用
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
    app.SetShowHud(false);   // 交付/验收图要干净；HUD 只给交互窗口截图用
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

// SSR 预设拍摄：固定光照，两个场景 × 三档参数。
//
// 为什么两个场景：
//   * 三方块场景 —— 原验收件（金属方块，三面各映不同内容）。
//   * **镜面地板场景** —— 专为 SSR 设计的"尺子"：地板是一个**完美平面**，倒影内容
//     一目了然（彩色方块、橡树、天空各在哪、有没有错位/断层）。以后接"平面反射真值"
//     也在这个场景里比。
//
// 三档：off（走天光镜面探针）/ hiz2_48（推荐默认）/ nohiz_64（质量上限，最慢）。
int RunCaptureSsr(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — SSR 预设对比（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetShowHud(false);   // 交付/验收图要干净；HUD 只给交互窗口截图用   // 交付/验收图要干净；HUD 只给交互窗口截图用
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    app.deg_.turbidity    = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 40.0f;
    app.deg_.sun_azim_deg = 30.0f;

    auto shot = [&](const char* name, bool on, int level, int steps) {
        jpov::SsrConfig sc;
        sc.enabled = on;
        sc.resolution_scale = 0.5f;
        sc.hiz_start_level = level;
        sc.max_steps = steps;
        app.SetSsr(sc);
        app.RunOnce(input, winfo, (out_dir + "/" + name + ".png").c_str());
        LOG(INFO) << "capture: " << name << " (on=" << on << " hiz=" << level
                  << " steps=" << steps << ")";
    };

    // ── 场景 A：三方块（3/4 俯视，三面都入画）──
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kThreeBoxes);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.42;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 4.6;
    shot("box_off",      false, 2, 48);
    shot("box_hiz2_48",  true,  2, 48);
    shot("box_nohiz_64", true,  0, 64);

    // ── 场景 B：镜面地板（低机位，地面占屏大、倒影一目了然）──
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kMirrorFloor);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.30;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 7.5;
    shot("floor_off",      false, 2, 48);
    shot("floor_hiz2_48",  true,  2, 48);
    shot("floor_nohiz_64", true,  0, 64);

    LOG(INFO) << "SSR presets captured to " << out_dir;
    app.Finalize();
    return 0;
}

// 复现"俯视角拉长"这一个具体工况（Danis 2026-10-26 截图给的机位）。
//
// 机位来自截图 HUD：phi=69.2 theta=1.7 R=9.50（近俯视），SSR = res 0.5 / HiZ 2 /
// steps 48 / thick 0.60 / rough<=0.40。四档对照用来定位是谁造成的：
//   case_off     关闭 SSR（纯探针）—— 如果拉长还在，就不是 SSR
//   case_hiz2    截图原参数
//   case_res1    同参数但反射缓冲全分辨率 —— 看是不是半分辨率/上采样造成的
//   case_nohiz   关 Hi-Z + 64 步 —— 看是不是步进/漏检造成的
int RunCaptureCase(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — SSR 典型工况复现（headless）";
    cfg.width  = jpov_skylight::kViewerWidth;
    cfg.height = jpov_skylight::kViewerHeight;
    cfg.headless = true;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.SetShowHud(false);
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kMirrorFloor);
    app.Init();
    InstallScene(app);
    InstallModels(app);
    InstallPerson(app);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    // 截图 HUD 的机位与天光
    app.deg_.turbidity    = 2.0f;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg = 45.0f;
    app.deg_.sun_azim_deg = 45.0f;
    app.view_.phi   = 69.2 * 3.14159265358979323846 / 180.0;
    app.view_.theta = 1.7 * 3.14159265358979323846 / 180.0;
    app.view_.R     = 9.50;

    auto shot = [&](const char* name, bool on, float scale, int level, int steps) {
        jpov::SsrConfig sc;
        sc.enabled = on;
        sc.resolution_scale = scale;
        sc.hiz_start_level = level;
        sc.max_steps = steps;
        app.SetSsr(sc);
        app.RunOnce(input, winfo, (out_dir + "/" + name + ".png").c_str());
        LOG(INFO) << "capture: " << name;
    };

    shot("case_off",   false, 0.5f, 2, 48);
    shot("case_hiz2",  true,  0.5f, 2, 48);
    shot("case_res1",  true,  1.0f, 2, 48);
    shot("case_nohiz", true,  0.5f, 0, 64);

    // DDA 对照（同一机位、同一权重链，只换行进算法）
    {
        jpov::SsrConfig sc;
        sc.enabled = true;
        sc.resolution_scale = 0.5f;
        sc.hiz_start_level = 2;
        sc.max_steps = 192;          // DDA 一步 = 1 像素，想看完整行程要调大
        sc.march_mode = 1;
        app.SetSsr(sc);
        app.RunOnce(input, winfo, (out_dir + "/case_dda.png").c_str());
        LOG(INFO) << "capture: case_dda";
    }

    // 俯视过渡序列：同一 SSR 参数，只把仰角往上抬。
    // 用来检查"往俯视走的过程中倒影会不会先被拉长再消失"（那是 transition 期的假命中）。
    for (int e : {30, 45, 58, 69}) {
        app.view_.phi = static_cast<double>(e) * 3.14159265358979323846 / 180.0;
        const std::string name = "tilt_" + std::to_string(e);
        shot(name.c_str(), true, 0.5f, 2, 48);
    }
    // Danis 给的"掠射近景"机位（phi=0.6 theta=27.8 R=1.68）：这里最容易漏光/拉丝。
    // 三档：关 SSR / Hi-Z / DDA，用来确认"漏光"是哪条链路贡献的。
    {
        app.view_.phi   = 0.6 * 3.14159265358979323846 / 180.0;
        app.view_.theta = 27.8 * 3.14159265358979323846 / 180.0;
        app.view_.R     = 1.68;
        shot("graze_off", false, 0.5f, 2, 48);
        shot("graze_hiz", true,  0.5f, 2, 48);
        jpov::SsrConfig sc;
        sc.enabled = true;
        sc.resolution_scale = 0.5f;
        sc.max_steps = 192;
        sc.march_mode = 1;
        app.SetSsr(sc);
        app.RunOnce(input, winfo, (out_dir + "/graze_dda.png").c_str());
        LOG(INFO) << "capture: graze_dda";
    }

    // 同一个 30° 机位走 DDA（这一步是"同权重链、只换行进算法"的关键对照）
    {
        app.view_.phi = 30.0 * 3.14159265358979323846 / 180.0;
        jpov::SsrConfig sc;
        sc.enabled = true;
        sc.resolution_scale = 0.5f;
        sc.max_steps = 192;
        sc.march_mode = 1;
        app.SetSsr(sc);
        app.RunOnce(input, winfo, (out_dir + "/tilt_30_dda.png").c_str());
        LOG(INFO) << "capture: tilt_30_dda";
    }

    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
    // 用法：jpov_skylight_viewer --capture /tmp/sky_out
    std::string out_dir;
    std::string smaa_out_dir;
    std::string env_out_dir;
    std::string ssr_out_dir;
    std::string case_out_dir;
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
        } else if (std::strcmp(argv[i], "--capture-case") == 0 && i + 1 < argc) {
            case_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }
    if (!case_out_dir.empty()) {
        return RunCaptureCase(case_out_dir);
    }
    if (!ssr_out_dir.empty()) {
        return RunCaptureSsr(ssr_out_dir);
    }
    if (!env_out_dir.empty()) {
        return RunCaptureEnv(env_out_dir);
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
    InstallPerson(app);

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

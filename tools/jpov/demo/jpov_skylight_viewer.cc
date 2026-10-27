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
    app.person_scale_ = scale;
    app.person_asset_min_y_ = min_y;

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

// 水面场景的“透射验证”道具：水下大 box → 小 box → 桌子 → 桌上的人。
// 布局与材质全在 skylight_water_props.h（单一出处）；这里只负责注册 mesh / 加载 glb。
void InstallWaterProps(jpov_skylight::SkylightApp& app) {
    app.water_big_box_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width=*/jpov_skylight::kWaterBigBoxHalfXZ,
        /*up_half_width=*/jpov_skylight::kWaterBigBoxHalfY,
        /*left_half_width=*/jpov_skylight::kWaterBigBoxHalfXZ));
    app.water_small_box_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width=*/jpov_skylight::kWaterSmallBoxHalfXZ,
        /*up_half_width=*/jpov_skylight::kWaterSmallBoxHalfY,
        /*left_half_width=*/jpov_skylight::kWaterSmallBoxHalfXZ));
    app.water_big_box_mat_   = jpov_skylight::WaterBigBoxMaterial();
    app.water_small_box_mat_ = jpov_skylight::WaterSmallBoxMaterial();

    // 桌子：**第二次加载同一份 glb**。不让它复用模型场景那一份 —— glTF 资源按
    // “每次 LoadGltf 独占”约定（interface/gltf_object.h），共享就得讨论释放权归谁。
    // 代价只是一张桌子的小网格，换掉一条隐式所有权。
    jpov::GltfObject table = app.LoadGltf(kTableModelPath);
    CHECK(!table.empty()) << "LoadGltf failed: " << kTableModelPath;
    float scale = 1.0f;
    float min_y = 0.0f;
    if (table.bounds_valid) {
        const float h = table.bounds_max[1] - table.bounds_min[1];
        if (h > 1.0e-6f) {
            scale = jpov_skylight::kWaterTableHeight / h;
        }
        min_y = table.bounds_min[1];
    }
    // 桌脚（资产局部 min.y）对齐小 box 顶面：center.y = 顶面 − scale·min.y。
    app.water_table_center_ = {0.0f, jpov_skylight::kWaterSmallBoxTopY - scale * min_y, 0.0f};
    app.water_table_scale_  = scale;
    app.water_table_       = std::move(table);
}

// 水面场景：一个人站在桌面上（脚 = 桌面高）。复用 InstallPerson 装配好的
// 同一份蒙皮 mesh / 骨架，只多一组摆放 —— 不多加载一次资产。
void InstallWaterPerson(jpov_skylight::SkylightApp& app) {
    CHECK_NE(app.person_mesh_, 0u) << "水面场景的站台人需要先 InstallPerson";
    CHECK_NE(app.person_skeleton_id_, 0u) << "水面场景的站台人需要先 InstallPerson";
    jpov::SkinnedInstanceState inst;
    inst.transform.center = {0.0f,
                             jpov_skylight::kWaterPersonFeetY
                                 - app.person_scale_ * app.person_asset_min_y_,
                             0.0f};
    inst.transform.up    = {0.0f, 1.0f, 0.0f};
    inst.transform.front = {0.0f, 0.0f, 1.0f};
    inst.transform.scale = app.person_scale_;
    inst.pose_a = 0;   // 唯一一帧 = identity pose（T-pose）
    inst.pose_b = 0;
    inst.ratio  = 0.0f;
    app.water_person_instances_.clear();
    app.water_person_instances_.push_back(inst);
}

// headless 拍摄：水面场景（strip 平铺的 10 km² 玻璃 quad）。
// 用法：jpov_skylight_viewer --capture-water <out_dir>
//
// 出图（同一套天光，只换机位 / 场景）：
//   water_eye_level   —— 眼高（y≈1.5 m）看出去：近处看得穿、远处被 fresnel 抬亮；
//   water_grazing     —— 更低（y≈0.5 m）：全身掠射，验收「水面收敛成一条亮带」；
//   water_mid_air     —— y≈25 m 俯视：看清水面是**一整块**平铺的面（无接缝/无洞）；
//   water_high        —— y≈1.5 km 高空斜视：验收 10 km² 的尺度与远端边界；
//   model_scene       —— 切回模型场景（证明模型场景仍在、与水面互斥）。
int RunCaptureWater(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 水面（strip 玻璃 quad）headless capture";
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
    InstallWaterProps(app);    // 水下大 box / 小 box + 桌子
    InstallWaterPerson(app);   // 桌上的人
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kWater);

    jpov::WindowInfo winfo;
    winfo.width  = jpov_skylight::kViewerWidth;
    winfo.height = jpov_skylight::kViewerHeight;
    jpov::InputSnapshot input{};

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // 固定天光：正午侧光（水面的 fresnel 亮带与天光颜色都能看清）。
    app.deg_.turbidity       = jpov_skylight::kTurbidityDef;
    app.deg_.daylight_season = 0.0f;
    app.deg_.sun_elev_deg    = 45.0f;
    app.deg_.sun_azim_deg    = 40.0f;

    // 机位表：phi 是仰角（弧度）、R 是到原点的距离。注意 headless 直接赋值
    // ⇒ **不受 ViewConfig::kRMax(300 m) 限制**（滚轮才会 clamp）。
    //
    // 每个机位都带一对 A/B（开 / 关水皮）：水下两件 box 在两张图里的差别
    // **就是透射本身的作用** —— 关水前后水下部分一模一样就意味着水皮没盖上去。
    struct Shot {
        const char* name;
        double phi;
        double R;
        bool water_scene = true;   // true = 水面场景（含道具塔）
        bool water_skin = true;    // 只在水面场景有意义：是否画水面
        bool use_strip = false;    // true = strip 平铺版（对照）；false = 新水皮 pass
        // 相机注视点的 y（米）；nullopt = 默认原点。水下抬头看水面时必须抬高，
        // 否则视线一直是水平的，而 Snell 窗在**头顶**（正上方 ±48.6°）。
        std::optional<float> target_y;
        // 水的折射率；0 = 用默认 1.333。
        float ior = 0.0f;
        // 波浪相位（弧度）；nullopt 的斜率 = 用 app 默认（10°），显式 0 = 关掉波纹。
        float wave_phase = 0.0f;
        std::optional<float> wave_slope_deg;
        // 强制透射下限（0 = 物理）。n 用上面的 ior 字段（0 = 默认 1.333）。
        float min_transmission = 0.0f;
        // 水上三项的 A/B（默认全开）：0 = 不覆盖，1 = 开，2 = 关。
        // 用 int 而不是 bool 是为了让"没写"和"显式关"能区分。
        int sky_refl = 0;
        int transmit = 0;
        int sun_glint = 0;
        int filter_mode = -1;
        // 相机方位角（度）。默认 45°；太阳高光要"相机与太阳关于竖直面对称"
        // （H = V+L 竖直 ⇒ N=up 才能对上），所以太阳在方位 40° 时相机要在 ~220°。
        float theta_deg = 45.0f;
    };
    const Shot shots[] = {
        {"props_eye", 0.30, 12.0},                                     // 眼高俯视（新 pass）
        {"props_eye_strip", 0.30, 12.0, true, true, /*use_strip=*/true},// 同机位 strip 版
        {"props_eye_nowater", 0.30, 12.0, true, /*water_skin=*/false},
        {"props_top", 0.70, 10.0},                                     // 陡俯视（正对水下）
        {"props_top_nowater", 0.70, 10.0, true, /*water_skin=*/false},
        {"props_grazing", 0.10, 14.0},                                 // 掠射（水线切在桌腿上）
        // 水下抬头：相机 y = −1.0（R·sinφ），视线抬到 ~48° / ~74°
        //   ring：视线切过临界角 ⇒ 窗内（透射）与窗外（地色）同屏；
        //   up  ：视线更接近竖直 ⇒ 以窗内透射为主。
        {"props_under_ring", -0.083, 12.0, true, true, false, 12.0f, 0.0f},
        {"props_under_up", -0.083, 12.0, true, true, false, 40.0f, 0.0f},
        // 同机位关掉折射（ior=1）—— 窗内变回"一刀切的原样天空"，用来对照折射到底改了什么。
        {"props_under_up_noref", -0.083, 12.0, true, true, false, 40.0f, 1.0f},
        // 水下平视偏上（视线 ~30°）：水上的人/桌子和窗同屏，用来看水面/窗缘的整体形状。
        {"props_under_wide", -0.083, 12.0, true, true, false, 6.0f, 0.0f},
        // 水纹（法线扰动）：同一机位三个相位 —— 相位滑条就是让这三张之间过渡的那根。
        {"props_under_wave_p0", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 0.0f, std::nullopt},
        {"props_under_wave_p1", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 1.6f, std::nullopt},
        {"props_under_wave_p2", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 3.2f, std::nullopt},
        // 关掉波纹的对照（法线恒为 +Y）——"这一屏的涟漪到底是不是波纹给的"由它来判。
        {"props_under_nowave", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 0.0f, 0.0f},
        // 水上视角：有水纹 / 无水纹。
        {"props_eye_wave", 0.30, 12.0, true, true, false, std::nullopt, 0.0f, 1.6f, std::nullopt},
        {"props_eye_nowave", 0.30, 12.0, true, true, false, std::nullopt, 0.0f, 0.0f, 0.0f},
        // n 滑条：1.333（物理水）→ 1.20 → 1.05 → 1.00（界面消失，全部透射）。
        {"props_under_n120", -0.083, 12.0, true, true, false, 12.0f, 1.20f, 0.0f, std::nullopt},
        {"props_under_n105", -0.083, 12.0, true, true, false, 12.0f, 1.05f, 0.0f, std::nullopt},
        {"props_under_n100", -0.083, 12.0, true, true, false, 12.0f, 1.00f, 0.0f, std::nullopt},
        // 最小透射滑条：物理 n 下强制留 20% / 50% 透射 ⇒ 灰区漏出天空。
        {"props_under_t20", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 0.0f, std::nullopt, 0.20f},
        {"props_under_t50", -0.083, 12.0, true, true, false, 12.0f, 0.0f, 0.0f, std::nullopt, 0.50f},
        // 水上三项 A/B（同一机位 = 水上视角）：全开 / 分别关掉一项。
        {"props_eye_3on", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt},
        {"props_eye_nosky", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 2, 0, 0},
        {"props_eye_notrans", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 2, 0},
        {"props_eye_nosun", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 2},
        // 同机位两个相位：用来量"远处到底还有没有波纹"（近/中/远三条带分别比）。
        {"props_eye_phaseB", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 1.6f, std::nullopt},
        // 四种"远距离带限"对比（同机位同相位，只换过滤方式）：
        //   0 不过滤（aliasing 基线）/ 1 硬关八度+归一化 / 2 逐八度 sinc / 3 sinc+方差转粗糙度
        {"filter0_alias", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 0, 45.0f},
        {"filter1_hard", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 1, 45.0f},
        {"filter2_sinc", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 2, 45.0f},
        {"filter3_lean", 0.42, 11.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 3, 45.0f},
        // 太阳高光机位下同样四种（看高光波瓣被方差拉宽的效果）。
        {"filtersun0_alias", 0.785, 10.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 0, 220.0f},
        {"filtersun3_lean", 0.785, 10.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, 3, 220.0f},
        // 太阳高光的机位：相机在**太阳方位**上、俯视 45°（镜像几何 V≈L ⇒ 高光落在画面中心）。
        // 顺带证明"高光不是恒亮的"：换机位它就该消失（那才是菲涅耳反射该有的行为）。
        {"props_eye_sunpath", 0.785, 10.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 0, -1, 220.0f},
        {"props_eye_sunpath_off", 0.785, 10.0, true, true, false, std::nullopt, 0.0f, 0.0f, std::nullopt, 0.0f, 0, 0, 2, -1, 220.0f},
        // 关掉波纹的太阳高光：应当收成一个**很尖的亮点**（波浪在的时候是被打碎的亮片）。
        {"props_eye_sunpath_nowave", 0.785, 10.0, true, true, false, std::nullopt, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, -1, 220.0f},
        // 判"透射到底透的是什么"：n=1（界面消失）应当 ≈ 完全不画水面（= 原样场景）。
        {"props_under_wide_n100", -0.083, 12.0, true, true, false, 6.0f, 1.00f, 0.0f, std::nullopt},
        {"props_under_wide_nowater", -0.083, 12.0, true, false, false, 6.0f, 0.0f, 0.0f, std::nullopt},
        {"water_high", 0.62, 2600.0},                                  // 10 km² 的尺度
        // 模型场景（回到默认机位）：证明「模型场景」还在，且与水面互斥。
        {"model_scene", 0.25, 9.5, /*water_scene=*/false},
    };
    for (const Shot& s : shots) {
        app.SetScene(s.water_scene ? jpov_skylight::SkylightApp::SceneKind::kWater
                                   : jpov_skylight::SkylightApp::SceneKind::kModel);
        app.SetShowWater(s.water_skin);
        app.SetWaterUseStrip(s.use_strip);
        app.SetWaterIor(s.ior > 0.0f ? s.ior : 1.333f);
        app.water_wave_phase_ = s.wave_phase;
        app.water_wave_slope_deg_ = s.wave_slope_deg.value_or(10.0f);
        app.water_min_transmission_ = s.min_transmission;
        if (s.filter_mode >= 0) {
            app.water_wave_filter_mode_ = s.filter_mode;
        }
        app.water_use_sky_refl_  = (s.sky_refl != 2);
        app.water_use_transmit_  = (s.transmit != 2);
        app.water_use_sun_glint_ = (s.sun_glint != 2);
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = s.phi;
        app.view_.theta = static_cast<double>(s.theta_deg) *
                          3.14159265358979323846 / 180.0;
        app.view_.R     = s.R;
        if (s.target_y.has_value()) {
            app.view_target_ = jpov::Vec3f(0.0f, *s.target_y, 0.0f);
        } else {
            app.view_target_.reset();
        }
        shoot(s.name);
    }
    app.view_target_.reset();
    app.SetShowWater(true);
    app.SetWaterUseStrip(false);

    // 面板形态（交互窗口的真实观感）：水面场景只应剩「场景」下拉 + 一行提示，
    // 模型场景仍是原来的满屏滑条 —— 两张对照图就是这条需求的验收工件。
    app.SetShowPanel(true);
    app.InstallTextMeasure();
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kWater);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.30;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 12.0;
    shoot("water_ui");
    app.SetScene(jpov_skylight::SkylightApp::SceneKind::kModel);
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.25;
    app.view_.theta = 3.14159265358979323846 / 4.0;
    app.view_.R     = 9.5;
    shoot("model_ui");
    app.SetShowPanel(false);

    app.Finalize();
    return 0;
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

int main(int argc, char** argv) {
    // ── headless 拍摄分支：--capture <out_dir> ──
    // 用法：jpov_skylight_viewer --capture /tmp/sky_out
    std::string out_dir;
    std::string smaa_out_dir;
    std::string env_out_dir;
    std::string water_out_dir;
    float smaa_threshold = 0.1f;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-smaa") == 0 && i + 1 < argc) {
            smaa_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-env") == 0 && i + 1 < argc) {
            env_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--capture-water") == 0 && i + 1 < argc) {
            water_out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--smaa-threshold") == 0 && i + 1 < argc) {
            smaa_threshold = std::strtof(argv[i + 1], nullptr);
        }
    }
    if (!water_out_dir.empty()) {
        return RunCaptureWater(water_out_dir);
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
    InstallWaterProps(app);
    InstallWaterPerson(app);

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

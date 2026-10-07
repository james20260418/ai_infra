// JPOV 穿衣工具 — 主程序（装配 + 模式分发）
//
// 用途：把「一件人体 reference」与「一件衣服模型」加载进同一个 JPOV 场景显示，
// 并让衣服被软体仿真器驱动（重力 + 顶点间弹簧力场 + 地面投影）。y-up，地平面
// 300×300 米高粗糙灰色 quad，光照固定 45° 天光，视角靠鼠标操作（右键 drag 转、滚轮 zoom）。
//
// 当前阶段（2026-10-02）：
//   ① 后台线程为两份 glb 建最近邻三角形匹配器（完成后主线程上传 GPU 资产）；
//   ② 衣服被 soft_mesh_simulator::Simulator 驱动，右上角面板调动力学系数 / 人体排斥 /
//      暂停 / 重置；左上角面板做平移 / 旋转 / 缩放（**即时**作用于仿真状态）+ 保存 glb。
//   仍不做：穿衣对齐 / 自动贴合。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov/clothing:jpov_clothing_tool -- --body_reference_path /path/to/body.glb --cloth_path /path/to/cloth.glb
//   或 sh 脚本：
//   ./tools/jpov/clothing_tool.sh --body_reference_path ... --cloth_path ...
//   → output/jpov_clothing_tool/jpov_clothing_tool ...

#include <algorithm>
#include <chrono>
#include <cstdlib>  // atof / atoi
#include <string>
#include <thread>

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_tool_app.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

// 命令行解析结果。
struct CliOptions {
    std::string body_reference_path;  // --body_reference_path 人体 reference（.glb/.gltf）
    std::string cloth_path;           // --cloth_path 衣服模型（.glb/.gltf）
    std::string output_dir;           // --ui_shot 的落盘目录（默认当前目录）
    bool  ui_shot = false;            // headless 单帧出图（含面板，UI 自检）
    int   sim_steps = 0;              // --sim_steps 出图前先推进的仿真步数（验证下坠/落地）
    int   window_width = 0;           // --window_width 覆盖窗口宽（0 = 用默认；验证 resize 布局）
    int   window_height = 0;          // --window_height 覆盖窗口高（0 = 用默认）
    float phi_deg = 20.0f;            // --phi_deg 初始俯视角（度；>0 = 相机在上方俯视）
    // 动力学初始值（默认 = Simulator 默认；仅 headless 调参 / 复现用）。
    float gravity = jpov::soft_mesh_simulator::Simulator::kDefaultGravity;
    float total_mass = jpov::soft_mesh_simulator::Simulator::kDefaultTotalMass;
    float force_coeff = jpov::soft_mesh_simulator::Simulator::kDefaultForceCoeff;
    float damping = jpov::soft_mesh_simulator::Simulator::kVelocityDamping;
    float max_speed = jpov::clothing::kDefaultMaxSpeed;  // --max_speed 全局速度上限（m/s；0 = 不限）
    int   body_repulsion = 1;         // --body_repulsion 0/1（人体排斥开关）
    float body_buffer = jpov::soft_mesh_simulator::Simulator::kDefaultBodyBuffer;  // --body_buffer（m）
    float body_parallel_damping =  // --body_parallel_damping（切向速度保留系数 0~1）
        jpov::soft_mesh_simulator::Simulator::kDefaultBodyParallelDamping;
    bool  has_cloth_offset = false;   // 是否给了 --cloth_offset
    float cloth_offset[3] = {0.0f, 0.0f, 0.0f};  // --cloth_offset dx dy dz（米）
    // 焊接判据（蒙皮面板的对应控件）：相对局部边长（比例）或绝对距离（mm）。
    bool  weld_relative = false;  // --weld_relative 0/1（1 = 相对局部边长）
    float weld_ratio = jpov::clothing::kSkinWeldRatio;  // --weld_ratio（相对模式比例）
    bool  auto_skin = false;      // --auto_skin：headless 出图前跑一次一键蒙皮（脚本化验证）
    // 随机摆动测试（headless / 脚本化验证；交互窗口由左下角面板驱动）。
    bool  motion_enable = false;  // --motion_enable：启用随机摆动测试
    float motion_phase = -1.0f;   // --motion_phase [0,1]（<0 = 未指定；给了即启用）
    float motion_amp = -1.0f;     // --motion_amp 度 [0,90]（<0 = 未指定）
};

// 解析 CLI：标志可任意位置；未知标志 → WARNING 忽略（不崩溃）。
CliOptions ParseCli(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ui_shot") {
            opt.ui_shot = true;
        } else if (arg == "--body_reference_path") {
            if (i + 1 < argc) {
                opt.body_reference_path = argv[++i];
            } else {
                LOG(WARNING) << "--body_reference_path 缺少路径参数，忽略";
            }
        } else if (arg == "--cloth_path") {
            if (i + 1 < argc) {
                opt.cloth_path = argv[++i];
            } else {
                LOG(WARNING) << "--cloth_path 缺少路径参数，忽略";
            }
        } else if (arg == "--output_dir") {
            if (i + 1 < argc) {
                opt.output_dir = argv[++i];
            } else {
                LOG(WARNING) << "--output_dir 缺少目录参数，忽略";
            }
        } else if (arg == "--sim_steps") {
            if (i + 1 < argc) {
                opt.sim_steps = std::atoi(argv[++i]);
                if (opt.sim_steps < 0) {
                    LOG(FATAL) << "--sim_steps 必须 >= 0，got " << opt.sim_steps;
                }
            } else {
                LOG(WARNING) << "--sim_steps 缺少数值参数，忽略";
            }
        } else if (arg == "--gravity") {
            if (i + 1 < argc) {
                opt.gravity = std::atof(argv[++i]);
                if (opt.gravity < 0.0f) {
                    LOG(FATAL) << "--gravity 必须 >= 0，got " << opt.gravity;
                }
            } else {
                LOG(WARNING) << "--gravity 缺少数值参数，忽略";
            }
        } else if (arg == "--total_mass") {
            if (i + 1 < argc) {
                opt.total_mass = std::atof(argv[++i]);
                if (!(opt.total_mass >= jpov::soft_mesh_simulator::Simulator::kMinTotalMass)) {
                    LOG(FATAL) << "--total_mass 必须 >= "
                               << jpov::soft_mesh_simulator::Simulator::kMinTotalMass
                               << "，got " << opt.total_mass;
                }
            } else {
                LOG(WARNING) << "--total_mass 缺少数值参数，忽略";
            }
        } else if (arg == "--force_coeff") {
            if (i + 1 < argc) {
                opt.force_coeff = std::atof(argv[++i]);
                if (!(opt.force_coeff > 0.0f)) {
                    LOG(FATAL) << "--force_coeff 必须 > 0，got " << opt.force_coeff;
                }
            } else {
                LOG(WARNING) << "--force_coeff 缺少数值参数，忽略";
            }
        } else if (arg == "--damping") {
            if (i + 1 < argc) {
                opt.damping = std::atof(argv[++i]);
                if (opt.damping < 0.0f) {
                    LOG(FATAL) << "--damping 必须 >= 0，got " << opt.damping;
                }
            } else {
                LOG(WARNING) << "--damping 缺少数值参数，忽略";
            }
        } else if (arg == "--max_speed") {
            if (i + 1 < argc) {
                opt.max_speed = std::atof(argv[++i]);
                if (opt.max_speed < 0.0f) {
                    LOG(FATAL) << "--max_speed 必须 >= 0，got " << opt.max_speed;
                }
            } else {
                LOG(WARNING) << "--max_speed 缺少数值参数，忽略";
            }
        } else if (arg == "--body_repulsion") {
            if (i + 1 < argc) {
                opt.body_repulsion = std::atoi(argv[++i]);
                if (opt.body_repulsion != 0 && opt.body_repulsion != 1) {
                    LOG(FATAL) << "--body_repulsion 必须是 0 或 1，got "
                               << opt.body_repulsion;
                }
            } else {
                LOG(WARNING) << "--body_repulsion 缺少数值参数，忽略";
            }
        } else if (arg == "--body_buffer") {
            if (i + 1 < argc) {
                opt.body_buffer = std::atof(argv[++i]);
                if (!(opt.body_buffer >= jpov::soft_mesh_simulator::Simulator::kMinBodyBuffer &&
                      opt.body_buffer <= jpov::soft_mesh_simulator::Simulator::kMaxBodyBuffer)) {
                    LOG(FATAL) << "--body_buffer 必须在 ["
                               << jpov::soft_mesh_simulator::Simulator::kMinBodyBuffer << ", "
                               << jpov::soft_mesh_simulator::Simulator::kMaxBodyBuffer
                               << "] 内，got " << opt.body_buffer;
                }
            } else {
                LOG(WARNING) << "--body_buffer 缺少数值参数，忽略";
            }
        } else if (arg == "--body_parallel_damping") {
            if (i + 1 < argc) {
                opt.body_parallel_damping = std::atof(argv[++i]);
                if (!(opt.body_parallel_damping >=
                          jpov::soft_mesh_simulator::Simulator::kMinBodyParallelDamping &&
                      opt.body_parallel_damping <=
                          jpov::soft_mesh_simulator::Simulator::kMaxBodyParallelDamping)) {
                    LOG(FATAL) << "--body_parallel_damping 必须在 ["
                               << jpov::soft_mesh_simulator::Simulator::kMinBodyParallelDamping
                               << ", "
                               << jpov::soft_mesh_simulator::Simulator::kMaxBodyParallelDamping
                               << "] 内，got " << opt.body_parallel_damping;
                }
            } else {
                LOG(WARNING) << "--body_parallel_damping 缺少数值参数，忽略";
            }
        } else if (arg == "--cloth_offset") {
            if (i + 3 < argc) {
                opt.cloth_offset[0] = std::atof(argv[++i]);
                opt.cloth_offset[1] = std::atof(argv[++i]);
                opt.cloth_offset[2] = std::atof(argv[++i]);
                opt.has_cloth_offset = true;
            } else {
                LOG(WARNING) << "--cloth_offset 需要 3 个数值（dx dy dz），忽略";
            }
        } else if (arg == "--auto_skin") {
            opt.auto_skin = true;
        } else if (arg == "--motion_enable") {
            opt.motion_enable = true;
        } else if (arg == "--motion_phase") {
            if (i + 1 < argc) {
                opt.motion_phase = static_cast<float>(std::atof(argv[++i]));
                if (opt.motion_phase < 0.0f || opt.motion_phase > 1.0f) {
                    LOG(FATAL) << "--motion_phase 必须在 [0,1]，got " << opt.motion_phase;
                }
            } else {
                LOG(WARNING) << "--motion_phase 缺少数值参数，忽略";
            }
        } else if (arg == "--motion_amp") {
            if (i + 1 < argc) {
                opt.motion_amp = static_cast<float>(std::atof(argv[++i]));
                if (opt.motion_amp < 0.0f ||
                    opt.motion_amp > jpov::clothing::kMotionAmplitudeMaxDeg) {
                    LOG(FATAL) << "--motion_amp 必须在 [0,"
                               << jpov::clothing::kMotionAmplitudeMaxDeg
                               << "]，got " << opt.motion_amp;
                }
            } else {
                LOG(WARNING) << "--motion_amp 缺少数值参数，忽略";
            }
        } else if (arg == "--weld_relative") {
            if (i + 1 < argc) {
                opt.weld_relative = (std::atoi(argv[++i]) != 0);
            } else {
                LOG(WARNING) << "--weld_relative 缺少数值参数，忽略";
            }
        } else if (arg == "--weld_ratio") {
            if (i + 1 < argc) {
                opt.weld_ratio = std::atof(argv[++i]);
            } else {
                LOG(WARNING) << "--weld_ratio 缺少数值参数，忽略";
            }
        } else if (arg == "--window_width") {
            if (i + 1 < argc) {
                opt.window_width = std::atoi(argv[++i]);
                if (opt.window_width <= 0) {
                    LOG(FATAL) << "--window_width 必须 > 0，got " << opt.window_width;
                }
            } else {
                LOG(WARNING) << "--window_width 缺少数值参数，忽略";
            }
        } else if (arg == "--window_height") {
            if (i + 1 < argc) {
                opt.window_height = std::atoi(argv[++i]);
                if (opt.window_height <= 0) {
                    LOG(FATAL) << "--window_height 必须 > 0，got " << opt.window_height;
                }
            } else {
                LOG(WARNING) << "--window_height 缺少数值参数，忽略";
            }
        } else if (arg == "--phi_deg") {
            if (i + 1 < argc) {
                opt.phi_deg = static_cast<float>(std::atof(argv[++i]));
                if (opt.phi_deg < -89.0f || opt.phi_deg > 89.0f) {
                    LOG(FATAL) << "--phi_deg 必须在 [-89,89]，got " << opt.phi_deg;
                }
            } else {
                LOG(WARNING) << "--phi_deg 缺少数值参数，忽略";
            }
        } else {
            LOG(WARNING) << "未知参数: " << arg << "; 已忽略";
        }
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    const CliOptions opt = ParseCli(argc, argv);

    // 人体 reference：未指定则退回项目内自带的 Mixamo 男性人体（便于快速跑通）。
    std::string body_path = opt.body_reference_path;
    if (body_path.empty()) {
        body_path = jpov::clothing::kDefaultBodyReferencePath;
        LOG(WARNING) << "未提供 --body_reference_path，使用演示人体 reference: "
                     << body_path;
    }
    // 衣服模型：没有通用默认值，必须显式指定（避免拿任意资产冒充衣服的隐式行为）。
    if (opt.cloth_path.empty()) {
        LOG(FATAL) << "必须用 --cloth_path 指定衣服模型（.glb/.gltf）。示例：\n"
                   << "  jpov_clothing_tool \\\n"
                   << "    --body_reference_path "
                   << jpov::clothing::kDefaultBodyReferencePath << " \\\n"
                   << "    --cloth_path /path/to/cloth.glb";
    }

    // ── 配置：初始窗口 1280×720、60fps。--ui_shot 走 headless（无可见窗口）。──
    // 注意：JPOV 窗口**可 resize**（框架未设 GLFW_RESIZABLE=GLFW_FALSE）；config.resizable
    // 未被框架消费，仅为语义声明。面板布局在 App 里按**每帧窗口尺寸** winfo 推算。
    const int cfg_w = opt.window_width > 0 ? opt.window_width
                                           : jpov::clothing::kDefaultWindowWidth;
    const int cfg_h = opt.window_height > 0 ? opt.window_height
                                            : jpov::clothing::kDefaultWindowHeight;
    JPOV::Config cfg;
    cfg.title = "JPOV — 穿衣工具";
    cfg.width  = cfg_w;
    cfg.height = cfg_h;
    cfg.resizable = true;
    cfg.target_fps = static_cast<int>(jpov::clothing::kViewerFps);
    cfg.headless   = opt.ui_shot;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov::clothing::ClothingToolApp app(cfg);
    app.SetShowPanel(!opt.ui_shot);  // 交互画面板；headless 单帧默认纯 3D
    app.Init();
    app.InstallTextMeasure();

    // ── 第一步：后台初始化（为两份 glb 建最近邻三角形匹配器）──
    // 路径先存到 App（面板/进度页要显示），再发起后台线程。
    app.body_path_  = body_path;
    app.cloth_path_ = opt.cloth_path;
    app.init_.Start(body_path, opt.cloth_path);

    // 场景静态资源只建一次（不在 OneIteration 里重复构造/上传）。
    app.ground_mat_  = jpov::clothing::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov::clothing::MakeGroundQuad());

    // 初始视角：默认目标原点、R 先取默认值；等 GPU 资产上传后由 App 按「人体 ∪ 衣服」
    // 包围盒自适应（退化则保持默认）。
    app.view_ = jpov::clothing::DefaultView();
    app.view_.phi = static_cast<double>(opt.phi_deg) * 3.14159265358979323846 / 180.0;

    // 动力学初值（CLI，可选）：在场景就绪（建仿真器）前设好镜像，使仿真器按此初始化。
    app.gravity_ui_ = opt.gravity;
    app.total_mass_ui_ = opt.total_mass;
    app.force_coeff_t_ui_ = jpov::clothing::ClothingToolApp::ForceNewtonToT(
        opt.force_coeff);
    app.damping_ui_ = opt.damping;
    app.max_speed_ui_ = opt.max_speed;
    app.body_repulsion_ui_ = (opt.body_repulsion != 0);
    app.body_buffer_ui_ = opt.body_buffer;
    app.body_parallel_damping_ui_ = opt.body_parallel_damping;
    app.weld_relative_ui_ = opt.weld_relative;
    app.weld_ratio_ = opt.weld_ratio;

    // 随机摆动测试初值（CLI，可选）：交互窗口由左下角面板驱动。
    app.motion_test_enabled_ = opt.motion_enable;
    if (opt.motion_phase >= 0.0f) {
        app.motion_test_enabled_ = true;  // 给了相位即启用（便于 headless 出图）
        app.motion_phase_ = opt.motion_phase;
    }
    if (opt.motion_amp >= 0.0f) {
        app.motion_amplitude_deg_ = opt.motion_amp;
    }

    if (opt.ui_shot) {
        // headless 单帧出图：无事件循环，需先把后台建图与 GPU 上传推进到就绪。
        // 反复调 TickInit（与交互循环同一条流程）直到场景就绪（GPU 资产已上传）。
        while (!app.TickInit()) {
            if (app.init_.state() == jpov::clothing::InitState::kFailed) {
                LOG(FATAL) << "后台初始化失败：" << app.init_.error_message();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // 可选：出图前先把衣服预摆位（headless 复现“穿衣服”用；等价面板平移）。
        if (opt.has_cloth_offset) {
            app.TranslateCloth(jpov::Vec3f(opt.cloth_offset[0], opt.cloth_offset[1],
                                           opt.cloth_offset[2]));
            LOG(INFO) << "衣服预平移 [" << opt.cloth_offset[0] << ","
                      << opt.cloth_offset[1] << "," << opt.cloth_offset[2] << "]";
        }
        // 可选：出图前跑一次一键蒙皮（脚本化验证焊接模式等；交互窗口由按钮驱动）。
        if (opt.auto_skin) {
            app.RunAutoSkinNow();
        }
        // 可选：出图前推进 N 步仿真（验证下坠 / 落地；交互窗口由「继续仿真」按钮驱动）。
        if (opt.sim_steps > 0) {
            app.AdvanceSimulationSteps(opt.sim_steps);
            for (const auto& sim : app.sims_) {
                const auto b = sim.Bounds();
                LOG(INFO) << "推进 " << opt.sim_steps << " 步后：包围盒 y ["
                          << b.min[1] << ", " << b.max[1] << "]，t=" << sim.time()
                          << "s，步=" << sim.step_count();
            }
        }

        // headless 单帧出图（带面板，UI 布局/字体自检用）。
        app.SetShowPanel(true);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(cfg_w);
        winfo.height = static_cast<float>(cfg_h);
        const std::string out_dir =
            opt.output_dir.empty() ? std::string(".") : opt.output_dir;
        const std::string out_path = out_dir + "/clothing_tool_ui.png";
        app.RunOnce(jpov::InputSnapshot(), winfo, out_path.c_str());
        LOG(INFO) << "UI 自检图已写入: " << out_path;
    } else {
        app.Run();  // 交互窗口事件循环（阻塞）
    }

    app.Finalize();
    return 0;
}

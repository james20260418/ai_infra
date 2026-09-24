// JPOV 软体仿真查看器 — 主程序（装配 + 模式分发）
//
// 用途：把「软体仿真器」（tools/jpov/soft_mesh_simulator/）产出的网格显示出来。
// 加载一个 glTF 模型（几何取自它的第一个 primitive，材质沿用它的第一份材质），
// 交给 soft_mesh_simulator::Simulator Init，然后每帧把「当前 mesh 经 1/60 s 动力学」
// 的结果画出来。y-up，地平面 300×300 米高粗糙灰色 quad，光照固定正午晴天。
//
// 与 jpov_model_viewer 的关系：同款骨架（view_config.h 的视角/光照/地面工具），
// 但**滑条只保留「地面高度」**（视角靠鼠标操作），删掉了太阳仰角/浊度/季节 R/
// 模型缩放——光照在本查看器里是固定标定值，不是被调对象。
//
// 当前阶段（M0）：动力学尚未实现，Simulator::Step 是恒等桩。
// 本查看器现在验证的是「静态展示模型」的能力（加载/摆放/光照/相机/UI 全链路）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_soft_mesh_viewer -- /path/to/model.glb
//   或 sh 脚本：
//   ./tools/jpov/build_soft_mesh_simulator.sh
//   → output/jpov_soft_mesh_viewer/jpov_soft_mesh_viewer <glb 路径>

#include <string>
#include <utility>  // std::move

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/soft_mesh_viewer_app.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"
#include "tools/jpov/src/gltf_loader.h"

namespace {

// 项目内可用的演示 glTF（未指定路径时 fallback，便于快速跑通）。
constexpr const char* kDefaultGltfPath =
    "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";

// LoadGltfScene 的回调是**函数指针**（不是 std::function，见 gltf_loader.h 的
// GltfMeshEntryCallback），故用「函数指针 + void* user_data」这一对表达「取第一条」。
// 只收第一条 primitive 作为被仿真网格（M0 单网格；多 primitive 后续再谈）。
struct FirstMeshSink {
    jpov::MeshData mesh;
    bool have = false;
};

void TakeFirstMesh(const jpov::GltfMeshEntry* entry, void* user_data) {
    auto* sink = static_cast<FirstMeshSink*>(user_data);
    if (sink->have) {
        return;  // 已有第一条，其余忽略（不拷贝）
    }
    sink->mesh = std::move(entry->mesh);  // entry 非 const → 可 Move 走
    sink->have = true;
}

// 命令行解析结果。
struct CliOptions {
    std::string gltf_path;
    std::string output_dir;  // --ui_shot 的落盘目录（默认当前目录）
    bool ui_shot = false;    // headless 单帧出图（含面板，UI 自检）
};

// 解析 CLI：标志可任意位置；第一个非 "--" 前缀参数 = glTF 路径；
// 未知标志 → WARNING 忽略（不崩溃）。
CliOptions ParseCli(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ui_shot") {
            opt.ui_shot = true;
        } else if (arg == "--output_dir") {
            if (i + 1 < argc) {
                opt.output_dir = argv[++i];
            } else {
                LOG(WARNING) << "--output_dir 缺少目录参数，忽略";
            }
        } else if (arg.rfind("--", 0) == 0) {
            LOG(WARNING) << "未知参数: " << arg << "; 已忽略";
        } else if (opt.gltf_path.empty()) {
            opt.gltf_path = arg;
        } else {
            LOG(WARNING) << "多余位置参数: " << arg << "; 已忽略";
        }
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    const CliOptions opt = ParseCli(argc, argv);

    std::string gltf_path = opt.gltf_path;
    if (gltf_path.empty()) {
        gltf_path = kDefaultGltfPath;
        LOG(WARNING) << "未提供 glTF 路径，使用演示模型: " << gltf_path;
    }

    // ── 配置：1280×720 不可 resize、60fps。--ui_shot 走 headless（无可见窗口）。──
    JPOV::Config cfg;
    cfg.title = "JPOV — 软体仿真查看器";
    cfg.width  = jpov::soft_mesh_viewer::kViewerWidth;
    cfg.height = jpov::soft_mesh_viewer::kViewerHeight;
    cfg.resizable = false;
    cfg.target_fps = static_cast<int>(jpov::soft_mesh_viewer::kViewerFps);
    cfg.headless   = opt.ui_shot;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov::soft_mesh_viewer::SoftMeshViewerApp app(cfg);
    app.SetShowPanel(!opt.ui_shot);  // 交互画面板；headless 单帧默认纯 3D
    app.Init();
    app.InstallTextMeasure();

    // ── 加载 glTF 的 CPU 几何（仿真器吃 MeshData，不吃 GltfObject）──
    // 用 LoadGltfScene 逐条交付，取第一个 primitive 作为被仿真的网格。
    FirstMeshSink sink;
    const bool scene_ok = jpov::LoadGltfScene(gltf_path, &TakeFirstMesh, &sink);
    CHECK(scene_ok) << "LoadGltfScene 失败: " << gltf_path;
    CHECK(sink.have) << "glTF 无任何 primitive（几何为空）: " << gltf_path;
    const jpov::MeshData sim_mesh = std::move(sink.mesh);

    // 材质：用引擎的 LoadGltf 载一份完整资产，取第一份材质（含上传后的贴图句柄）。
    // 说明：几何走 CPU 侧 LoadGltfScene（仿真器要 MeshData），材质走 LoadGltf
    // （要的是纹理句柄 + PBRMaterial），两条路径各取所需。
    const jpov::GltfObject asset = app.LoadGltf(gltf_path);
    CHECK(!asset.empty()) << "LoadGltf 失败或模型为空: " << gltf_path;
    app.mesh_mat_ = asset.primitives.front().material;

    // 组装场景。
    app.ground_mat_  = jpov::soft_mesh_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov::soft_mesh_viewer::MakeGroundQuad());

    // 仿真网格注册进 MeshManager（上传一次；此后每步 UpdateMesh 推新顶点）。
    app.mesh_id_ = app.RegisterMesh(sim_mesh);

    // 仿真器初始化（绑定姿态 = 这份网格；Reset 会回到它）。
    app.sim_.Init(sim_mesh);

    // 初始视角：目标原点、R 按 CPU 包围盒自适应（退化时退回 DefaultView）。
    app.view_ = jpov::soft_mesh_viewer::DefaultView();
    const jpov::soft_mesh_simulator::SimBounds bounds = app.sim_.Bounds();
    if (bounds.valid) {
        const float bmin[3] = {bounds.min[0], bounds.min[1], bounds.min[2]};
        const float bmax[3] = {bounds.max[0], bounds.max[1], bounds.max[2]};
        app.view_.R = jpov::soft_mesh_viewer::ViewConfig::FitRadius(bmin, bmax, 60.0);
        LOG(INFO) << "仿真网格包围盒 [" << bmin[0] << "," << bmin[1] << ","
                  << bmin[2] << "] ~ [" << bmax[0] << "," << bmax[1] << ","
                  << bmax[2] << "]，初始 R=" << app.view_.R;
    }

    if (opt.ui_shot) {
        // headless 单帧出图（带面板，UI 布局/字体自检用）。
        app.SetShowPanel(true);
        jpov::WindowInfo winfo;
        winfo.width  = jpov::soft_mesh_viewer::kViewerWidth;
        winfo.height = jpov::soft_mesh_viewer::kViewerHeight;
        const std::string out_dir =
            opt.output_dir.empty() ? std::string(".") : opt.output_dir;
        const std::string out_path = out_dir + "/soft_mesh_viewer_ui.png";
        app.RunOnce(jpov::InputSnapshot(), winfo, out_path.c_str());
        LOG(INFO) << "UI 自检图已写入: " << out_path;
    } else {
        app.Run();  // 交互窗口事件循环（阻塞）
    }

    app.Finalize();
    return 0;
}

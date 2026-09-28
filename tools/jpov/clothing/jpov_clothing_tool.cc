// JPOV 穿衣工具 — 主程序（装配 + 模式分发）
//
// 用途：把「一件人体 reference」与「一件衣服模型」加载进同一个 JPOV 场景并显示，
// 为后续「衣服贴合人体」的穿衣管线提供可视化底座。y-up，地平面 300×300 米高粗糙
// 灰色 quad，光照固定正午晴天，视角靠鼠标操作（右键 drag 转、滚轮 zoom）。
//
// 本阶段（M0）**只做「加载 + 显示」**：不做对齐、不做穿衣物理。详见
// clothing_tool_app.h 的边界说明。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov/clothing:jpov_clothing_tool -- --body_reference_path /path/to/body.glb --cloth_path /path/to/cloth.glb
//   或 sh 脚本：
//   ./tools/jpov/clothing_tool.sh --body_reference_path ... --cloth_path ...
//   → output/jpov_clothing_tool/jpov_clothing_tool ...

#include <algorithm>
#include <cstdlib>  // atof
#include <string>

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_tool_app.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

// 命令行解析结果。
struct CliOptions {
    std::string body_reference_path;  // --body_reference_path 人体 reference（.glb/.gltf）
    std::string cloth_path;           // --cloth_path 衣服模型（.glb/.gltf）
    std::string output_dir;           // --ui_shot 的落盘目录（默认当前目录）
    bool  ui_shot = false;            // headless 单帧出图（含面板，UI 自检）
    float phi_deg = 20.0f;            // --phi_deg 初始俯视角（度；>0 = 相机在上方俯视）
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

// 人体 reference + 衣服两个资产包围盒的并集（相机自适应用）。
// 任一资产为空时只取另一份；两份都空时 valid=false。
struct BoundsUnion {
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
    bool valid = false;
};

void AccumulateBounds(const jpov::GltfObject& obj, BoundsUnion* out /*inout*/) {
    if (!obj.bounds_valid) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (!out->valid) {
            out->min[i] = obj.bounds_min[i];
            out->max[i] = obj.bounds_max[i];
        } else {
            out->min[i] = std::min(out->min[i], obj.bounds_min[i]);
            out->max[i] = std::max(out->max[i], obj.bounds_max[i]);
        }
    }
    out->valid = true;
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

    // ── 配置：1280×720 不可 resize、60fps。--ui_shot 走 headless（无可见窗口）。──
    JPOV::Config cfg;
    cfg.title = "JPOV — 穿衣工具";
    cfg.width  = jpov::clothing::kViewerWidth;
    cfg.height = jpov::clothing::kViewerHeight;
    cfg.resizable = false;
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

    // ── 加载两个资产（各自整份画；M0 静态，不需要 CPU 侧形变）。──
    app.body_ = app.LoadGltf(body_path);
    CHECK(!app.body_.empty())
        << "人体 reference 加载失败或为空: " << body_path
        << "（请确认路径存在且为合法 .gltf/.glb）";
    app.cloth_ = app.LoadGltf(opt.cloth_path);
    CHECK(!app.cloth_.empty())
        << "衣服模型加载失败或为空: " << opt.cloth_path
        << "（请确认路径存在且为合法 .gltf/.glb）";
    app.body_path_  = body_path;
    app.cloth_path_ = opt.cloth_path;

    // 场景静态资源只建一次（不在 OneIteration 里重复构造/上传）。
    app.ground_mat_  = jpov::clothing::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov::clothing::MakeGroundQuad());

    // 初始视角：目标原点、R 按「人体 ∪ 衣服」包围盒自适应（退化时退回 DefaultView）。
    app.view_ = jpov::clothing::DefaultView();
    app.view_.phi = static_cast<double>(opt.phi_deg) * 3.14159265358979323846 / 180.0;
    BoundsUnion bounds;
    AccumulateBounds(app.body_, &bounds);
    AccumulateBounds(app.cloth_, &bounds);
    if (bounds.valid) {
        app.view_.R = jpov::clothing::ViewConfig::FitRadius(bounds.min, bounds.max,
                                                            /*fov_deg*/ 60.0);
        LOG(INFO) << "场景包围盒 [" << bounds.min[0] << "," << bounds.min[1] << ","
                  << bounds.min[2] << "] ~ [" << bounds.max[0] << "," << bounds.max[1]
                  << "," << bounds.max[2] << "]，初始 R=" << app.view_.R;
    }
    LOG(INFO) << "人体 reference: " << body_path << "（" << app.body_.size()
              << " primitives）";
    LOG(INFO) << "衣服模型: " << opt.cloth_path << "（" << app.cloth_.size()
              << " primitives）";

    if (opt.ui_shot) {
        // headless 单帧出图（带面板，UI 布局/字体自检用）。
        app.SetShowPanel(true);
        jpov::WindowInfo winfo;
        winfo.width  = jpov::clothing::kViewerWidth;
        winfo.height = jpov::clothing::kViewerHeight;
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

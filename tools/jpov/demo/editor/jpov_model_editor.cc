// JPOV 模型编辑器 — 主程序（装配 + 模式分发）
//
// 用途（retarget 前置组件）：加载一个 glTF/GLB 资产，用人手把它微调到与目标
// 骨架对齐的姿态——缩放、平移、旋转。本 PR 只做"摆"，**不做资产保存**；
// 摆放数学（model_placement.h）产出的 (center,up,front,scale) 就是后续 PR 反推
// "顶点调整量 / 骨长比例" 的输入。
//
// 与 jpov_model_viewer 的关系：同一套场景与相机（sunny day 光照 / 可调地面 /
// 右键环绕），面板换成放置控件，另消费左键横向 drag 做旋转。详见 editor_app.h。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_model_editor -- /absolute/path/to/model.glb
//   或 sh 脚本：
//   ./tools/jpov/build_jpov_model_editor.sh
//   → output/jpov_model_editor/jpov_model_editor <gltf/glb 路径>

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/editor/editor_app.h"
#include "tools/jpov/demo/view_config.h"

namespace {

// 项目内可用的演示 glTF（命令行未指定路径时的 fallback，便于快速跑通）。
// 复用查看器同款演示资产（plier 小工具），保证 editor 开箱即可看到东西。
std::string DefaultGltfPath() {
    return "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
}

// 取模型路径：遍历 argv[1..] 的第一个非 "--" 前缀参数（与查看器同款约定）。
// 本工具目前只有交互模式（无 headless 子命令），故不需要模式枚举；
// 未来加拍摄/批处理模式下再引入 RunMode（YAGNI，不过早抽象）。
std::string ParseGltfPath(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            LOG(WARNING) << "未知参数: " << arg << "; 已忽略";
        } else if (path.empty()) {
            path = arg;
        } else {
            LOG(WARNING) << "多余位置参数: " << arg << "; 已忽略";
        }
    }
    return path;
}

}  // namespace

int main(int argc, char** argv) {
    std::string gltf_path = ParseGltfPath(argc, argv);
    if (gltf_path.empty()) {
        gltf_path = DefaultGltfPath();
        LOG(WARNING) << "未提供 glTF 路径，使用演示模型: " << gltf_path;
    }

    // ── 配置：1280×720 不可 resize、60fps、交互窗口（本 PR 无 headless 模式）──
    JPOV::Config cfg;
    cfg.title = "JPOV — 模型编辑器（放置微调）";
    cfg.width  = jpov_viewer::kEditorWidth;
    cfg.height = jpov_viewer::kEditorHeight;
    cfg.resizable = false;
    cfg.target_fps = static_cast<int>(jpov_viewer::kEditorFps);
    cfg.headless = false;
    // 显式声明字体（CJK 显中文标签，Latin 回退；路径相对 exe 的 fonts/，
    // build_jpov_model_editor.sh 同构拷贝）。
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",            0, jpov::kFontBuiltinLatin},
    };

    jpov_viewer::EditorApp app(cfg);
    app.InstallTextMeasure();
    app.Init();

    // 加载模型到中心；失败 → FATAL 带清晰信息，不静默。
    app.gltf_ = app.LoadGltf(gltf_path);
    CHECK(!app.gltf_.empty())
        << "LoadGltf 失败或模型为空: " << gltf_path
        << "（请确认路径存在且为合法 .gltf/.glb）";

    // 场景静态资源只建一次。
    app.ground_mat_  = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad(
        jpov_viewer::kEditorGroundDefault));

    // 初始视角：目标原点、R 按模型包围盒自适应（退化时退回 DefaultView）。
    app.view_ = jpov_viewer::DefaultView();
    if (app.gltf_.bounds_valid) {
        app.view_.R = jpov_viewer::ViewConfig::FitRadius(
            app.gltf_.bounds_min, app.gltf_.bounds_max, /*fov_deg*/ 60.0);
        LOG(INFO) << "模型包围盒 [" << app.gltf_.bounds_min[0] << ","
                  << app.gltf_.bounds_min[1] << "," << app.gltf_.bounds_min[2]
                  << "] ~ [" << app.gltf_.bounds_max[0] << ","
                  << app.gltf_.bounds_max[1] << "," << app.gltf_.bounds_max[2]
                  << "]，初始 R=" << app.view_.R;
    }

    // 交互事件循环（阻塞）。
    app.Run();
    app.Finalize();
    return 0;
}

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

#include <optional>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/editor/editor_app.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/gltf_saver.h"

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

// 用纯 loader 加载一份 **CPU 快照**（几何 + 材质贴图路径 + 可选骨架），供保存线程使用。
//
// 为什么另读一份：Renderer::LoadGltf 上传 GPU 后不保留 CPU 几何（见 mesh_manager.h），
// 而保存需要 CPU 顶点。用纯 loader 读一份是最干净的做法（GL-free、与渲染零耦合）。
// 失败不致命（编辑器仍可交互），但会禁掉保存（save.has_asset() == false）。
void LoadCpuSnapshot(const std::string& path,
                     jpov_viewer::SaveController* save) {
    std::vector<jpov::GltfSaveMesh> meshes;
    struct Ctx {
        std::vector<jpov::GltfSaveMesh>* out;
    } ctx{&meshes};
    auto cb = [](const jpov::GltfMeshEntry* e, void* user) {
        Ctx* c = static_cast<Ctx*>(user);
        jpov::GltfSaveMesh sm;
        sm.mesh = e->mesh;
        sm.material = e->material;
        c->out->push_back(std::move(sm));
    };
    if (!jpov::LoadGltfScene(path, cb, &ctx) || meshes.empty()) {
        LOG(ERROR) << "CPU 快照加载失败（保存将不可用）: " << path;
        return;
    }

    std::optional<jpov::SkeletonType> skin;
    std::vector<jpov::SkeletonType> skins;
    if (jpov::LoadGltfSkeleton(path, &skins) && !skins.empty()) {
        skin = skins.front();   // 单骨架假设（与 viewer/editor 一致）
        LOG(INFO) << "CPU 快照：带骨 " << skin->bone_count() << " 关节";
    }

    // 取 basename 作资产名（glTF 无“模型名”字段时的稳定默认）。
    const size_t slash = path.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = stem.substr(0, dot);
    }

    save->SetAsset(std::move(meshes), std::move(skin), stem);
    LOG(INFO) << "CPU 快照就绪：" << save->primitive_count() << " 个 primitive";
}

}  // namespace

int main(int argc, char** argv) {
    std::string gltf_path = ParseGltfPath(argc, argv);
    if (gltf_path.empty()) {
        gltf_path = DefaultGltfPath();
        LOG(WARNING) << "未提供 glTF 路径，使用演示模型: " << gltf_path;
    }

    // ── 配置：默认 1280×720、60fps、交互窗口（本 PR 无 headless 模式）──
    // resizable = true：布局已改为随窗口尺寸自适应（面板贴左下、说明贴左上），
    // 放大窗口后滑条/文字会跟着重新贴边，不会飘。
    JPOV::Config cfg;
    cfg.title = "JPOV — 模型编辑器（放置微调）";
    cfg.width  = jpov_viewer::kEditorDefaultWidth;
    cfg.height = jpov_viewer::kEditorDefaultHeight;
    cfg.resizable = true;
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

    // 保存用 CPU 快照 + 原始路径（保存时输出到同目录）。
    app.source_gltf_path_ = gltf_path;
    LoadCpuSnapshot(gltf_path, &app.save_);

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

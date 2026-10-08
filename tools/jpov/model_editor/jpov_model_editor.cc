// JPOV 模型编辑器 — 主程序（装配 + 模式分发）
//
// 用途：加载 reference（可空）与 target 两个模型，在同一个 JPOV 场景显示；对 target
//   做平移 / 旋转 / 缩放（左上角面板）与裁剪（右侧面板，沿坐标平面 X/Y/Z 删一侧），
//   结果可保存成 glb。详见 model_editor_app.h。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   ./tools/jpov/model_editor.sh   （产物在 output/jpov_model_editor/）
//   运行：jpov_model_editor --target_path /path/to/target.glb
//         [--reference_path /path/to/reference.glb]
//
// headless UI 自检（不弹窗，出单张带面板的图）：
//   加 --ui_shot --output_dir /tmp/ui
//   加 [--clip_axis x|y|z --clip_coord V --clip_side delete_low|delete_high]
//   加 --save_after_clip [--save_format glb|gltf]（默认 glb；gltf = 外置纹理，可编辑）

#include <cstdlib>  // atof / atoi
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/model_editor/model_editor_app.h"

namespace {

struct CliOptions {
    std::string target_path;     // --target_path（必填）
    std::string reference_path;  // --reference_path（可选）
    std::string output_dir;      // --ui_shot 落盘目录（默认当前目录）
    bool ui_shot = false;
    float phi_deg = 20.0f;
    int window_width = 0;
    int window_height = 0;
    bool has_clip = false;       // 是否给了 --clip_side（在出图前裁一刀）
    int clip_axis = 1;           // --clip_axis x|y|z（默认 y）
    float clip_coord = 0.0f;     // --clip_coord（米）
    bool has_clip_coord = false; // 是否给了 --clip_coord（否则用该轴范围中点）
    jpov::model_editor::ClipKeepSide clip_side =
        jpov::model_editor::ClipKeepSide::kGreater;  // --clip_side 删除哪侧
    bool save_after_clip = false;  // --save_after_clip：出图前把（裁剪后）target 存盘
    std::string save_format = "glb";  // --save_format glb|gltf（gltf = 外置纹理）
};

// 解析 CLI：标志可任意位置；未知标志 WARNING 忽略。
CliOptions ParseCli(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ui_shot") {
            opt.ui_shot = true;
        } else if (arg == "--save_after_clip") {
            opt.save_after_clip = true;
        } else if (arg == "--save_format") {
            if (i + 1 < argc) {
                const std::string v = argv[++i];
                if (v == "glb" || v == "gltf") {
                    opt.save_format = v;
                } else {
                    LOG(FATAL) << "--save_format 仅支持 glb|gltf，got " << v;
                }
            } else {
                LOG(WARNING) << "--save_format 缺少参数（glb|gltf），忽略";
            }
        } else if (arg == "--target_path") {
            if (i + 1 < argc) {
                opt.target_path = argv[++i];
            } else {
                LOG(WARNING) << "--target_path 缺少路径参数，忽略";
            }
        } else if (arg == "--reference_path") {
            if (i + 1 < argc) {
                opt.reference_path = argv[++i];
            } else {
                LOG(WARNING) << "--reference_path 缺少路径参数，忽略";
            }
        } else if (arg == "--output_dir") {
            if (i + 1 < argc) {
                opt.output_dir = argv[++i];
            } else {
                LOG(WARNING) << "--output_dir 缺少目录参数，忽略";
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
        } else if (arg == "--clip_axis") {
            if (i + 1 < argc) {
                const std::string s = argv[++i];
                if (s == "x" || s == "X") {
                    opt.clip_axis = 0;
                } else if (s == "y" || s == "Y") {
                    opt.clip_axis = 1;
                } else if (s == "z" || s == "Z") {
                    opt.clip_axis = 2;
                } else {
                    LOG(FATAL) << "--clip_axis 必须是 x|y|z，got " << s;
                }
            } else {
                LOG(WARNING) << "--clip_axis 缺少取值参数，忽略";
            }
        } else if (arg == "--clip_coord") {
            if (i + 1 < argc) {
                opt.clip_coord = static_cast<float>(std::atof(argv[++i]));
                opt.has_clip_coord = true;
            } else {
                LOG(WARNING) << "--clip_coord 缺少数值参数，忽略";
            }
        } else if (arg == "--clip_side") {
            if (i + 1 < argc) {
                const std::string s = argv[++i];
                if (s == "delete_low") {
                    opt.clip_side = jpov::model_editor::ClipKeepSide::kGreater;
                } else if (s == "delete_high") {
                    opt.clip_side = jpov::model_editor::ClipKeepSide::kLess;
                } else {
                    LOG(FATAL) << "--clip_side 必须是 delete_low|delete_high，got " << s;
                }
                opt.has_clip = true;
            } else {
                LOG(WARNING) << "--clip_side 缺少取值参数，忽略";
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

    if (opt.target_path.empty()) {
        LOG(FATAL) << "必须用 --target_path 指定 target 模型（.glb/.gltf）。示例：\n"
                   << "  jpov_model_editor \\\n"
                   << "    --target_path /path/to/target.glb \\\n"
                   << "    [--reference_path /path/to/reference.glb]";
    }

    const int cfg_w = opt.window_width > 0 ? opt.window_width
                                           : jpov::model_editor::kDefaultWindowWidth;
    const int cfg_h = opt.window_height > 0 ? opt.window_height
                                            : jpov::model_editor::kDefaultWindowHeight;
    JPOV::Config cfg;
    cfg.title = "JPOV — 模型编辑器（裁剪）";
    cfg.width = cfg_w;
    cfg.height = cfg_h;
    cfg.resizable = true;
    cfg.target_fps = static_cast<int>(jpov::model_editor::kViewerFps);
    cfg.headless = opt.ui_shot;
    cfg.fonts = {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };

    jpov::model_editor::ModelEditorApp app(cfg);
    app.SetShowPanel(!opt.ui_shot);
    app.Init();
    app.InstallTextMeasure();

    app.target_path_ = opt.target_path;
    app.reference_path_ = opt.reference_path;
    app.has_reference_ = !opt.reference_path.empty();
    app.SetInitialViewPhi(static_cast<double>(opt.phi_deg) * 3.14159265358979323846 /
                          180.0);
    app.LoadScene();

    // headless 出图前可选：按 CLI 裁一刀 +（可选）存盘，便于脚本化验证。
    if (opt.ui_shot) {
        if (opt.has_clip) {
            app.SetClipAxis(opt.clip_axis);
            if (opt.has_clip_coord) {
                app.SetClipCoord(opt.clip_coord);
            }
            app.ApplyClip(opt.clip_side);
            LOG(INFO) << "headless 预裁剪：axis=" << opt.clip_axis
                      << " coord=" << opt.clip_coord
                      << " side=" << (opt.clip_side == jpov::model_editor::ClipKeepSide::kGreater
                                          ? "delete_low(保留大侧)"
                                          : "delete_high(保留小侧)");
        }
        if (opt.save_after_clip) {
            app.SaveTargetToSourceSync(
                opt.save_format == "gltf"
                    ? jpov::model_editor::ModelOutputFormat::kGltfExternal
                    : jpov::model_editor::ModelOutputFormat::kGlb);
            LOG(INFO) << "headless 存盘完成（format=" << opt.save_format << "）";
        }
        app.SetShowPanel(true);
        jpov::WindowInfo winfo;
        winfo.width = static_cast<float>(cfg_w);
        winfo.height = static_cast<float>(cfg_h);
        const std::string out_dir =
            opt.output_dir.empty() ? std::string(".") : opt.output_dir;
        const std::string out_path = out_dir + "/model_editor_ui.png";
        app.RunOnce(jpov::InputSnapshot(), winfo, out_path.c_str());
        LOG(INFO) << "UI 自检图已写入: " << out_path;
    } else {
        app.Run();
    }

    app.Finalize();
    // 等待后台保存线程收尾（若有），避免进程退出时线程仍在写文件。
    app.WaitForSave();
    return 0;
}

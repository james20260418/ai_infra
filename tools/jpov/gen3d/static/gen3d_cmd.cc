// JPOV gen3d — 命令行入口
//
// 第一个可执行 elf：接收参数，自动化调用 tripo3d API 生成静态 PBR 模型
// GLB 并落盘到指定 output_dir。日志打 stdout 明确产物绝对路径（防 confuse）。
//
// 用法（三种输入方式互斥，选一种）：
//   # A. 文本驱动（原有）
//   gen3d_cmd generate --name <slug> --output_dir <dir> --prompt "<文本>"
//   # B. 单图驱动
//   gen3d_cmd generate --name <slug> --output_dir <dir> --image <path>
//   # C. 多视图驱动（2~4 张，顺序 [front, left, back, right]）
//   gen3d_cmd generate --name <slug> --output_dir <dir> \
//       --images <front> <left> [<back>] [<right>]
//
//   通用可选：
//       [--negative "<排除>"]     # 仅文本模式
//       [--high_poly]             # 关低模默认
//       [--triangles <n>]         # 面数预算（低模默认 4000；-1=自适应）
//       [--real_size]             # 按真实尺寸（米）输出
//       [--align_to_image]        # 图像模式：把模型对齐到参考图视角
//       [--skip_image_check]      # 跳过本地图片审查（危险：可能白烧 credit，仅供调试）
//
// 【图片本地审查】图像模式下，提交前必须过 image_input.h 的审查（格式/大小/分辨率）。
// 不通过则**不发任何 HTTP 请求**、直接非零退出——避免白烧 credit。
// 图片路径**由调用方声明**（本工具不自动扫目录/不猜文件名）。
//
// API key 走环境变量 TRIPO_API_KEY（严禁硬编码/入仓库）。
// 成功后 stdout 打印一行 glb 绝对路径；失败非零退出并打印错误原因。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/gen3d/static/gen3d_config.h"
#include "tools/jpov/gen3d/static/image_input.h"
#include "tools/jpov/gen3d/static/tripo_client.h"

namespace {

using jpov::Gen3dConfig;                 // gen3d_config.h：jpov:: 命名空间
using jpov::Gen3dInputMode;
using jpov::gen3d::Gen3dResult;
using jpov::gen3d::TripoClient;

// 输出帮助到 stderr，返回 1（非正常终止路径统一给非零）。
void PrintUsage() {
    std::fprintf(stderr,
        "用法（三种输入方式互斥，选一种）：\n"
        "  gen3d_cmd generate --name <slug> --output_dir <dir> --prompt \"...\"\n"
        "  gen3d_cmd generate --name <slug> --output_dir <dir> --image <图片路径>\n"
        "  gen3d_cmd generate --name <slug> --output_dir <dir> \\\n"
        "      --images <front> <left> [<back>] [<right>]\n"
        "通用可选：\n"
        "  [--negative \"...\"]    仅文本模式\n"
        "  [--high_poly]          关低模默认\n"
        "  [--triangles <n>]      面数预算（低模默认 4000；-1=自适应）\n"
        "  [--real_size]          按真实尺寸（米）输出\n"
        "  [--align_to_image]     图像模式：模型对齐到参考图视角\n"
        "  [--skip_image_check]   跳过本地图片审查（危险，仅供调试）\n"
        "图片要求：PNG/JPG/WebP；≤ 20MB；每边 ≥ 256px；至少 2 张（多视图）；\n"
        "多视图顺序固定 [front, left, back, right]，front 不可省。\n"
        "环境变量 TRIPO_API_KEY 必须已设置（调 tripo3d 用）。\n"
        "成功后 stdout 打印一行 .glb 绝对路径；失败非零退出。\n");
}

// 取命令行 flag 值：--<name> <value>。value 是紧邻下一参数（非 -- 前缀）。
// 找不到返回 false；value 输出到 out。
bool GetFlagValue(int argc, char** argv, const std::string& name,
                  std::string* out /*output*/) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--" + name) {
            const std::string v = argv[i + 1];
            if (v.rfind("--", 0) == 0) {
                LOG(WARNING) << "--" << name << " 缺少值（下个参数是 " << v << "）";
                return false;
            }
            *out = v;
            return true;
        }
    }
    return false;
}

// 是否带某布尔 flag（--<name>，出现在任意位置）。
bool HasFlag(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--" + name) return true;
    }
    return false;
}

// 收集多值 flag：--<name> v1 v2 ...（直到下一个 -- 开头或参数尾）。
// 返回收集到的值列表（空 = 未出现或未跟任何值）。
std::vector<std::string> GetFlagValues(int argc, char** argv,
                                       const std::string& name) {
    std::vector<std::string> values;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--" + name) {
            continue;
        }
        for (int j = i + 1; j < argc; ++j) {
            const std::string v = argv[j];
            if (v.rfind("--", 0) == 0) {
                break;  // 遇到下一个 flag，收集结束
            }
            values.push_back(v);
        }
        break;  // 只认第一次出现的 --<name>
    }
    return values;
}

// 图像模式的本地审查门禁。返回 true = 可以提交。
// 审查不过则打印逐张报告并返回 false（调用方据此不发 HTTP，省 credit）。
bool CheckImagesForSubmit(const std::vector<std::string>& paths,
                          bool is_multiview) {
    std::fprintf(stderr, "==> 本地图片审查（避免白烧 credit）\n");
    bool all_ok = true;
    if (is_multiview) {
        // 多视图：走组合审查（含“至少 2 张 + front 必填”规则）。
        const std::vector<jpov::gen3d::ImageCheckResult> results =
            jpov::gen3d::CheckImageFiles(paths);
        for (size_t i = 0; i < results.size() && i < paths.size(); ++i) {
            const std::string report =
                jpov::gen3d::FormatImageCheckReport(paths[i], results[i]);
            std::fputs(report.c_str(), stderr);
            if (!results[i].ok) {
                all_ok = false;
            }
        }
    } else {
        // 单图：只审查这一张，**不套用多视图的数量规则**。
        jpov::gen3d::ImageCheckResult r;
        for (const std::string& p : paths) {
            r = jpov::gen3d::CheckImageFile(p);
            const std::string report =
                jpov::gen3d::FormatImageCheckReport(p, r);
            std::fputs(report.c_str(), stderr);
            if (!r.ok) {
                all_ok = false;
            }
        }
    }
    if (!all_ok) {
        std::fprintf(stderr,
            "审查未通过：已阻止提交（未消耗任何 credit）。请修正后重试。\n");
        return false;
    }
    if (is_multiview) {
        std::fprintf(stderr,
            "审查通过（%zu 张，顺序按 [front, left, back, right] 解释）。\n",
            paths.size());
    } else {
        std::fprintf(stderr, "审查通过（单图）。\n");
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }
    if (std::string(argv[1]) != "generate") {
        LOG(ERROR) << "未知子命令: " << argv[1];
        PrintUsage();
        return 1;
    }

    // ---- 读必填参数 ----
    std::string name, output_dir, prompt;
    if (!GetFlagValue(argc, argv, "name", &name) || name.empty()) {
        LOG(ERROR) << "缺少必填参数 --name <slug>";
        PrintUsage();
        return 1;
    }
    if (!GetFlagValue(argc, argv, "output_dir", &output_dir) || output_dir.empty()) {
        LOG(ERROR) << "缺少必填参数 --output_dir <dir>";
        PrintUsage();
        return 1;
    }

    // ---- 判定输入方式（--prompt / --image / --images 三者互斥）----
    const bool has_prompt = GetFlagValue(argc, argv, "prompt", &prompt) && !prompt.empty();
    std::string single_image;
    const bool has_image =
        GetFlagValue(argc, argv, "image", &single_image) && !single_image.empty();
    const std::vector<std::string> multi_images =
        GetFlagValues(argc, argv, "images");

    const int mode_count = (has_prompt ? 1 : 0) + (has_image ? 1 : 0)
        + (multi_images.empty() ? 0 : 1);
    if (mode_count == 0) {
        LOG(ERROR) << "必须提供 --prompt / --image / --images 之一";
        PrintUsage();
        return 1;
    }
    if (mode_count > 1) {
        LOG(ERROR) << "--prompt / --image / --images 互斥，只能给一种";
        PrintUsage();
        return 1;
    }

    Gen3dConfig config;
    if (has_prompt) {
        config.input_mode = Gen3dInputMode::kText;
        config.prompt = prompt;
    } else if (has_image) {
        config.input_mode = Gen3dInputMode::kSingleImage;
        config.input_image_paths = {single_image};
    } else {
        config.input_mode = Gen3dInputMode::kMultiview;
        config.input_image_paths = multi_images;
        if (config.input_image_paths.size() < 2) {
            LOG(ERROR) << "--images 至少需要 2 张（顺序 front/left/back/right）";
            PrintUsage();
            return 1;
        }
        if (config.input_image_paths.size() > 4) {
            LOG(ERROR) << "--images 最多 4 张（顺序 front/left/back/right）";
            PrintUsage();
            return 1;
        }
    }

    std::string neg;
    if (GetFlagValue(argc, argv, "negative", &neg) && !neg.empty()) {
        if (!has_prompt) {
            LOG(WARNING) << "--negative 仅文本模式有意义，图像模式下忽略";
        } else {
            config.negative_prompt = neg;
        }
    }
    config.align_to_image = HasFlag(argc, argv, "align_to_image");
    // 缺省 low_poly=true（JPOV 静态 PBR 资产默认走 tripo P1 低模，快且足够好看）。
    // --high_poly 反向关闭它（本次仍映射 P1，高模 H 档为后续扩展）。
    config.low_poly = !HasFlag(argc, argv, "high_poly");
    config.real_size = HasFlag(argc, argv, "real_size");
    std::string tri;
    const bool have_triangles =
        GetFlagValue(argc, argv, "triangles", &tri) && !tri.empty();
    if (have_triangles) {
        config.max_triangles = std::atoi(tri.c_str());
    } else if (config.low_poly) {
        // gen3d_config.h 结构体默认 low_poly=false 故首值 max_triangles=100000；
        // 运行时设 low_poly=true 不会追溯更新它。这里显式给低模默认 4000。
        config.max_triangles = 4000;
    }

    // ---- 图像模式的本地审查（提交前，避免白烧 credit）----
    // 注：审查放在读 API key 之前，使“图片不合格”在无 key 时也能报出。
    if (config.input_mode != Gen3dInputMode::kText) {
        if (HasFlag(argc, argv, "skip_image_check")) {
            LOG(WARNING) << "已跳过本地图片审查（--skip_image_check）：可能白烧 credit";
        } else {
            const bool is_mv = (config.input_mode == Gen3dInputMode::kMultiview);
            if (!CheckImagesForSubmit(config.input_image_paths, is_mv)) {
                return 1;
            }
        }
    }

    // ---- API key（环境变量）----
    const char* key_c = std::getenv("TRIPO_API_KEY");
    if (key_c == nullptr || std::string(key_c).empty()) {
        LOG(ERROR) << "环境变量 TRIPO_API_KEY 未设置（调 tripo3d 需 API key）";
        return 1;
    }
    const std::string api_key = key_c;
    TripoClient client(api_key);

    // 打印将生成什么，方便现场对照。
    const char* mode_str = has_prompt ? "text"
        : (has_image ? "single_image" : "multiview");
    LOG(INFO) << "将生成 model → output_dir=" << output_dir
              << " name=" << name
              << " mode=" << mode_str
              << " low_poly=" << (config.low_poly ? "yes" : "no")
              << " max_triangles=" << config.max_triangles;

    const Gen3dResult result = client.Generate(config, output_dir, name);
    if (result.glb_path.empty()) {
        LOG(ERROR) << "生成失败: " << result.error;
        return 1;
    }

    // 明确打印产物绝对路径（供脚本/用户直取，防 confuse）。
    LOG(INFO) << "生成成功，GLB 产物: " << result.glb_path;
    std::printf("%s\n", result.glb_path.c_str());
    return 0;
}

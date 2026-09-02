// JPOV gen3d — 命令行入口
//
// 第一个可执行 elf：接收参数，自动化调用 tripo3d API 生成静态 PBR 模型
// GLB 并落盘到指定 output_dir。日志打 stdout 明确产物绝对路径（防 confuse）。
//
// 用法：
//   gen3d_cmd generate [
//       --name <slug>            # 产物 basename（不含扩展名，落盘 <name>.glb）
//       --output_dir <dir>       # 产物父目录（不存在会自动建）
//       --prompt "<文本描述>"     # 必填；含空格/引号请用引号包裹
//       [--negative "<排除>"]    # 可选
//       [--high_poly]           # 关闭低模默认（默认低模 P1；high_poly 本次仍映射 P1，H 档为后续扩展）
//       [--triangles <n>]        # 面数预算（默认按 gen3d-config low_poly=4000；-1=自适应）
//       [--real_size]            # 按真实尺寸（米）输出（供 AR/场景等比）
//
// API key 走环境变量 TRIPO_API_KEY（严禁硬编码/入仓库）。
// 成功后 stdout 打印一行 glb 绝对路径；失败非零退出并打印错误原因。
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/gen3d/gen3d_config.h"
#include "tools/jpov/gen3d/tripo_client.h"

namespace {

using jpov::Gen3dConfig;                 // gen3d_config.h：jpov:: 命名空间
using jpov::gen3d::Gen3dResult;
using jpov::gen3d::TripoClient;

// 输出帮助到 stderr，返回 1（非正常终止路径统一给非零）。
void PrintUsage() {
    std::fprintf(stderr,
        "用法:\n"
        "  gen3d_cmd generate --name <slug> --output_dir <dir> --prompt \"...\"\n"
        "      [--negative \"...\"] [--high_poly] [--triangles <n>]\n"
        "      [--real_size]\n"
        "环境变量 TRIPO_API_KEY 必须已设置（调 tripo3d 用）。\n"
        "缺省低模模式（tripo P1，4000 面）；--high_poly 关闭低模默认。\n"
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
    if (!GetFlagValue(argc, argv, "prompt", &prompt) || prompt.empty()) {
        LOG(ERROR) << "缺少必填参数 --prompt \"...\"";
        PrintUsage();
        return 1;
    }

    Gen3dConfig config;
    config.prompt = prompt;
    std::string neg;
    if (GetFlagValue(argc, argv, "negative", &neg) && !neg.empty()) {
        config.negative_prompt = neg;
    }
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
        // 运行时设 low_poly=true 不会追溯更新它。这里显式给低模默认 4000
        // （tripo P1 推荐面数预算；见 gen3d_config.h 的设计注释）。
        config.max_triangles = 4000;
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
    LOG(INFO) << "将生成 model → output_dir=" << output_dir
              << " name=" << name
              << " low_poly=" << (config.low_poly ? "yes" : "no")
              << " max_triangles=" << config.max_triangles;

    const Gen3dResult result = client.GenerateTextToModel(
        config, output_dir, name);
    if (result.glb_path.empty()) {
        LOG(ERROR) << "生成失败: " << result.error;
        return 1;
    }

    // 明确打印产物绝对路径（供脚本/用户直取，防 confuse）。
    LOG(INFO) << "生成成功，GLB 产物: " << result.glb_path;
    std::printf("%s\n", result.glb_path.c_str());
    return 0;
}

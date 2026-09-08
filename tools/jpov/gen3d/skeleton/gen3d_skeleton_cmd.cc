// JPOV gen3d/skeleton — 带骨骼生成命令行入口
//
// 自动化调用 tripo3d 生成"带 mixamo 兼容骨骼的人形/生物模型"并落盘到
// output_dir。两步 task 链：kTextToModel 先 text-to-model 生成一个静态模型、
// 再对它 Auto Rig(spec=mixamo)；或 kDirectTask 直接对已有 task_id rig。
//
// 用法：
//   gen3d_skeleton_cmd generate --name <slug> --output_dir <dir> --prompt "..."
//       [--negative "..."] [--input_task_id <task_xxx>] [--spec tripo|mixamo]
//       [--rig_type biped|quadruped|...]
//
//   --name / --output_dir / --prompt 必填（除非用 --input_task_id 直连）。
//   --input_task_id：给则跳过 text-to-model，直接对该任务 rig（与 --prompt 互斥二选一）。
//   --spec        默认 mixamo（产出 mixamorig 前缀骨骼）；tripo=原生骨名。
//   --rig_type    默认 biped（人形）；可选 quadruped/hexapod/octopod/avian/serpentine/aquatic。
// API key 走环境变量 TRIPO_API_KEY（严禁硬编码/入仓库）。
// 成功后 stdout 打印一行带骨骼 GLB 绝对路径；失败非零退出。
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/gen3d/skeleton/rig_config.h"
#include "tools/jpov/gen3d/skeleton/skeleton_rig.h"

namespace {

using jpov::gen3d::RigConfig;
using jpov::gen3d::RigInputMode;
using jpov::gen3d::RigSpec;
using jpov::gen3d::RigType;
using jpov::gen3d::RigResult;
using jpov::gen3d::TripoRigClient;

void PrintUsage() {
    std::fprintf(stderr,
        "用法:\n"
        "  gen3d_skeleton_cmd generate --name <slug> --output_dir <dir>\n"
        "      [--prompt \"...\"] [--input_task_id <task_xxx>]\n"
        "      [--negative \"...\"] [--spec tripo|mixamo] [--rig_type <t>]\n"
        "说明:\n"
        "  --name/--output_dir 必填。\n"
        "  默认模式: 给 --prompt, 先 text-to-model 生成静态模型再对它 rig(mixamo)。\n"
        "  也支持 --input_task_id（跳过静态生成, 直接对已有模型任务 rig）——与 --prompt 二选一。\n"
        "  --spec 默认 mixamo（mixamorig 骨名）；tripo=原生骨名。\n"
        "  --rig_type 默认 biped；可选 quadruped/hexapod/octopod/avian/serpentine/aquatic。\n"
        "环境变量 TRIPO_API_KEY 必须已设置。\n"
        "成功后 stdout 打印带骨骼 GLB 绝对路径；失败非零退出。\n");
}

// 取 --<name> <value>。
bool GetFlagValue(int argc, char** argv, const std::string& name,
                  std::string* out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--" + name) {
            const std::string v = argv[i + 1];
            if (v.rfind("--", 0) == 0) {
                LOG(WARNING) << "--" << name << " 缺值（下个参数 " << v << "）";
                return false;
            }
            *out = v;
            return true;
        }
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

    std::string name, output_dir, prompt;
    if (!GetFlagValue(argc, argv, "name", &name) || name.empty()) {
        LOG(ERROR) << "缺少必填 --name <slug>";
        PrintUsage();
        return 1;
    }
    if (!GetFlagValue(argc, argv, "output_dir", &output_dir) || output_dir.empty()) {
        LOG(ERROR) << "缺少必填 --output_dir <dir>";
        PrintUsage();
        return 1;
    }

    RigConfig config;
    std::string input_task_id;
    GetFlagValue(argc, argv, "input_task_id", &input_task_id);
    GetFlagValue(argc, argv, "prompt", &prompt);
    if (!prompt.empty() && !input_task_id.empty()) {
        LOG(ERROR) << "--prompt 与 --input_task_id 互斥（二选一：自包含生成 or 直连现有模型）";
        return 1;
    }
    if (input_task_id.empty() && prompt.empty()) {
        LOG(ERROR) << "需给 --prompt（自包含生成带骨骼模型）或 --input_task_id（对已有模型 rig）";
        PrintUsage();
        return 1;
    }
    if (!input_task_id.empty()) {
        config.input_mode = RigInputMode::kDirectTask;
        config.input_task_id = input_task_id;
    } else {
        config.input_mode = RigInputMode::kTextToModel;
        config.prompt = prompt;
        std::string neg;
        if (GetFlagValue(argc, argv, "negative", &neg) && !neg.empty()) {
            config.negative_prompt = neg;
        }
    }

    // spec（默认 mixamo），大小写不敏感
    std::string spec_str;
    GetFlagValue(argc, argv, "spec", &spec_str);
    if (!spec_str.empty()) {
        for (auto& c : spec_str) c = static_cast<char>(::tolower(c));
        if (spec_str == "mixamo") config.spec = RigSpec::kMixamo;
        else if (spec_str == "tripo") config.spec = RigSpec::kTripo;
        else { LOG(ERROR) << "未知 --spec: " << spec_str << "（tripo|mixamo）"; return 1; }
    }

    // rig_type（默认 biped）
    std::string rt_str;
    GetFlagValue(argc, argv, "rig_type", &rt_str);
    if (!rt_str.empty()) {
        std::string m = rt_str;
        for (auto& c : m) c = static_cast<char>(::tolower(c));
        if (m == "quadruped") config.rig_type = RigType::kQuadruped;
        else if (m == "hexapod") config.rig_type = RigType::kHexapod;
        else if (m == "octopod") config.rig_type = RigType::kOctopod;
        else if (m == "avian") config.rig_type = RigType::kAvian;
        else if (m == "serpentine") config.rig_type = RigType::kSerpentine;
        else if (m == "aquatic") config.rig_type = RigType::kAquatic;
        else if (m == "biped") config.rig_type = RigType::kBiped;
        else { LOG(ERROR) << "未知 --rig_type: " << rt_str; return 1; }
    }

    // API key
    const char* key_c = std::getenv("TRIPO_API_KEY");
    if (key_c == nullptr || std::string(key_c).empty()) {
        LOG(ERROR) << "环境变量 TRIPO_API_KEY 未设置";
        return 1;
    }

    TripoRigClient client(key_c);
    const RigResult res = client.GenerateRigged(config, output_dir, name);
    if (res.glb_path.empty()) {
        LOG(ERROR) << "生成带骨骼模型失败: " << res.error;
        return 1;
    }
    LOG(INFO) << "生成成功，带骨骼 GLB 产物: " << res.glb_path;
    std::printf("%s\n", res.glb_path.c_str());
    return 0;
}

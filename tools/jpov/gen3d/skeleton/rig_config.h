// JPOV gen3d/skeleton — 带骨骼 3D 模型生成配置（供应商无关 RigConfig）
//
// 对应 gen3d/static 里 Gen3dConfig 的"静态生成"语义；这里是"带骨骼(rig)"语义。
// 设计原则延续 gen3d_config.h：本结构体只表达【用户想生成什么样的带骨骼模型】，
// 不绑定供应商字段。各 client（TripoRigClient / ...）内部把 RigConfig 映射成
// 供应商请求参数。换供应商只新增同接口 client，调用链不变。
//
// 目标链路（2026-09-08 权威核实，见 docs/jpov_clothes_rig_design.md）：
//   生成一具带 mixamo 兼容骨骼的人形/生物模型。Tripo v3 是 task 链式：
//     ┌─ 简单模式（自包含，本工具默认）：内部先 text-to-model 生成一个静态模型，
//     │   拿到 task_id 后立即对同一 task 调 Auto Rig（spec=mixamo）→ 带骨骼 GLB。
//     └─ 直连模式：用户已有一个模型 task_id，跳过"静态生成"，直接对它 rig。
//   最终落盘：output_dir/<name>.glb（带 mixamorig 骨骼 + 蒙皮权重，可被动画驱动）。
//
// 命名空间：gen3d 相关统一 jpov::gen3d（与 gen3d/static tripo_client 一致）。

#ifndef JPOV_GEN3D_SKELETON_RIG_CONFIG_H_
#define JPOV_GEN3D_SKELETON_RIG_CONFIG_H_

#include <string>

namespace jpov {
namespace gen3d {

// Rig 方式：本工具如何得到"即将被装上骨架"的输入模型。
enum class RigInputMode {
    kTextToModel,   // 自包含：给 prompt，内部先生成静态模型再 rig（默认，主机定位）
    kDirectTask,    // 直连：用户已有一个模型 task_id，跳过静态生成直接 rig
};

// 骨架命名规范 spec（供应商无关语义，Tripo 映射 mixamo/tripo）。
enum class RigSpec {
    kMixamo,   // Mixamo 兼容 bone naming（每根骨 mixamorig: 前缀，本项目目标）
    kTripo,    // Tripo 原生 bone naming
};

// 骨架类型 rig_type（biped 人形 / 四足 / ...）。用户一般只关心 biped 或"自动"。
enum class RigType {
    kBiped,      // 人形，两足（本项目主张；Tripo model v1.0 只支持 biped）
    kQuadruped,  // 四足
    kHexapod,    // 六足
    kOctopod,    // 八足
    kAvian,      // 鸟 / 有翼
    kSerpentine, // 蛇形
    kAquatic,    // 水生
};

// 一次"生成带骨骼模型"任务配置（供应商无关）。
struct RigConfig {
    // ---- 输入（按 input_mode 二选一生效）----
    // kTextToModel：描述想生成的模型/角色的 prompt。越具体越好（含角色因素：
    //   如 male / wearing underwear / bald / 希望体表能看清骨骼走向 之类）。
    // kDirectTask：用户已有一个 tripo 模型 task_id（"task_..."），直接对之 rig。
    std::string prompt;
    std::string negative_prompt;  // kTextToModel 模式下传给 text-to-model 的排除词
    std::string input_task_id;

    RigInputMode input_mode = RigInputMode::kTextToModel;

    // ---- 骨架语义 ----
    RigSpec spec = RigSpec::kMixamo;   // 默认产出 mixamo 兼容骨名（本项目目标）
    RigType rig_type = RigType::kBiped;  // 默认人形
    // 骨架 model version。用户可不指定，client 按 rig_type 映射 tripo 常量：
    //   biped → v1.0-20240301（只支持人形）；其余 → v2.5-20260210。
    // 若本字段非空则显式覆盖默认映射（给高级用户逃生口）。
    std::string rig_model_version;

    // ---- 输出 ----
    // true = 生成后顺手对输出做 Auto Rig? 否——本工具 RigConfig 语义就是
    //   "生成带骨骼"，不存在"不做 rig"的分支。此字段占位备用，勿改。
};

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_SKELETON_RIG_CONFIG_H_

// JPOV 3D 模型自动生产工具 — 生成配置
//
// gen3d 工具通过第三方 3D 生成 API（Tripo / Meshy / ...）按文本 prompt
// 生成静态 PBR 3D 模型（GLB），落盘到 git 忽略的 output/gen3d/ 下，
// 供后续 JPOV 渲染验证。
//
// 【设计原则：供应商无关】
//   本结构体只表达【用户想生成什么样的模型】这类通用语义，不绑定任何
//   具体供应商的字段名。每个供应商的 client（TripoClient / MeshyClient /
//   ...）在内部把 Gen3dConfig 映射成各自的请求参数，并在内部保证固定的
//   管线约束。因此换供应商时，config 与所有调用点无需改动。
//
// 【固定管线约束】不在此暴露，由各 client 内部写死：
//   - 恒 PBR（base_color / metallic / roughness / normal 四张贴图）
//   - 恒不透明（无 alpha）——JPOV 静态渲染不支持 transmission/透明材质
//   - 恒有贴图 + 恒有法线 + 恒有 UV（JPOV 静态 PBR 的硬性前置）
//   - 恒三角面（不自 quad，避免某些供应商强制转 FBX）
//   - 恒输出未压缩 GLB（回到 JPOV tinygltf loader 可直接解析）
//   - 恒输出 .glb 格式
//
// 【供应商专属参数】不在此暴露：模型版本号、随机种子、质量档的具体取值
//   等，由各 client 内部根据自身能力映射（或取保守默认）。

#ifndef JPOV_GEN3D_GEN3D_CONFIG_H_
#define JPOV_GEN3D_GEN3D_CONFIG_H_

#include <string>

namespace jpov {

// 一次"文本 → 3D"生成任务的配置（供应商无关）。
struct Gen3dConfig {
    // ---- 输入 ----
    // 描述目标物件的形状 / 材质 / 风格 / 大致尺寸。越具体越好，
    // 建议携带材质与风格线索（利于 PBR 贴图质量）。≤1024 字符。
    std::string prompt;

    // 不希望出现在模型里的内容（如 "blurry, broken mesh"）。≤255 字符。
    std::string negative_prompt;

    // ---- 生成风格 ----
    // 低模美术模式（smart low-poly）：true = 让生成器直接产出干净拓扑、
    // 低面数、独立部件的美术低模（stylized / game-ready low poly）。
    // false = 标准高模（自适应拓扑，高密度网格）。
    // 这是一个【算法/模式开关】，不是 prompt 里的文本暗示——只有它才能
    // 真正强制低面数干净拓扑，也可据此做面数断言校验。
    // 语义通用：Tripo→smart_low_poly / Meshy→model_type:smart-topology。
    bool low_poly = false;

    // ---- 几何约束 ----
    // 三角形数量目标/上限。语义上等价于"面数预算"，由各供应商 client 内部
    // 映射到对应参数（Tripo: face_limit / Meshy: target_polycount）。
    // 注：供应商面数总是"近似值"，实际可能略有超出/不足，不能当作硬上限。
    // 两种模式下的含义：
    //   low_poly = false：作"高模上限"，约定如 ≤100_000 游戏就绪。
    //   low_poly = true ：作"目标低模面数"，受 smart_low_poly 区间限制
    //                     （Tripo: 三角 500~20,000），默认取 4,000。
    //   -1          不限制（用供应商默认自适应拓扑）。
    int max_triangles = (low_poly ? 4000 : 100000);

    // ---- 尺寸语义 ----
    // 模型输出的尺寸基准：
    //   real_size = true  → 模型按物理真实尺寸生成。供应商用 AI 视觉估算
    //                       物件的真实大小（如"一张 1 米的椅子"），或遵循
    //                       prompt 中描述的尺寸，输出以米为单位的实际尺度。
    //                       适合需要物体间真实比例（如椅 0.9m 配桌 1.2m）的场景。
    //   real_size = false → 模型被归一化到 1 左右的规范尺寸（默认约 0~1 或
    //                       -1~1 立方体内），不带物理语义。无论 prompt 物件
    //                       实际多大，初始 bbox 都大致落在同一尺度，便于
    //                       统一摆场 / 统一相机；后续再各自缩放到目标大小。
    //   默认 false：即便必须调尺寸，也容易预测（每件初始尺度一致）。
    bool real_size = false;
};

}  // namespace jpov

#endif  // JPOV_GEN3D_GEN3D_CONFIG_H_

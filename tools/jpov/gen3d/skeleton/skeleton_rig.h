// JPOV gen3d/skeleton — Tripo3D 带骨骼生成 REST 客户端（RigClient）
//
// TripoRigClient 是 gen3d/skeleton 工具链里「唯一直接发 HTTP / 解析 tripo3d
// 动画/rig 响应」的类（延续 static tripo_client 的供应商无关设计——config
// 只表达用户想生成什么，本 client 内部把 RigConfig 映射成 tripo3d v3 请求）。
//
// 与 gen3d/static 的关系：
//   static 的 TripoClient 把"text-to-model → 下载 .glb"封死成单方法且不返回
//   task_id（rig 需要中间 task_id），因此 skeleton 需要一个能走"两步 task 链"
//   的独立 client。HTTP/轮询/下载在此自实现（参考 static 已验证的 curl 模式），
//   不侵入已稳定的 static；换供应商只需新增同接口 client。
//
// Tripo v3 契约（2026-09-08 权威核实 developers.tripo3d.ai）：
//   task 链（每一步提交后要 GET /tasks/{id} 轮询）：
//   1) Auto Rig    POST {base}/animations/rig
//         body: { "input":"<静态模型 task_id 或直链URL>",
//                 "model":"v2.5-20260210",        // 或 v1.0-20240301(biped)
//                 "rig_type":"biped", "spec":"mixamo", "out_format":"glb" }
//         → data.task_id（rig 任务）
//   2) 轮询该任务 → success 时 data.output.model_url（带骨骼 GLB 签名下载）
//   3) 下载 model_url → output_dir/<name>.glb（签名 URL，不带 Authorization）
//   说明：input 支持 task_id 或公开直链 URL。本工具主打"text-to-model 先生成
//   静态、再对它的 task_id rig"的自包含两步；也支持用户直接给已有 task_id。
//
// 命名空间：gen3d 相关统一 jpov::gen3d。

#ifndef JPOV_GEN3D_SKELETON_SKELETON_RIG_H_
#define JPOV_GEN3D_SKELETON_SKELETON_RIG_H_

#include <string>

#include "tools/jpov/gen3d/skeleton/rig_config.h"

namespace jpov {
namespace gen3d {

// 一次"生成带骨骼模型"任务结果（仅暴露调用方关心的通用语义，同 static Gen3dResult）。
struct RigResult {
    // 生成的带骨骼 GLB 落盘绝对路径；失败时为空。
    std::string glb_path;
    // 失败原因（空 = 成功）。供上层打印，不吞错误。
    std::string error;
};

// Tripo3D v3 带骨骼生成客户端。
class TripoRigClient {
public:
    // api_key 从环境变量 TRIPO_API_KEY 读取（严禁硬编码/入仓库）。
    // base_url 缺省 = https://openapi.tripo3d.ai/v3（全球端点）。子类/测试可注入。
    explicit TripoRigClient(const std::string& api_key,
                            const std::string& base_url =
                                "https://openapi.tripo3d.ai/v3");

    // 生成带骨骼模型 → 下载 GLB 到 <output_dir>/<name>.glb。
    // 按 config.input_mode：
    //   kTextToModel：内部提交 tripo text-to-model 拿到静态模型，再对同一任务
    //                 Auto Rig(spec=mixamo) → 下载带骨骼 GLB（自包含，本工具主打）。
    //   kDirectTask ：跳过静态生成，直接用 config.input_task_id 提交 Auto Rig。
    // 成功时 result.glb_path = 绝对路径；失败置 result.error。
    // 会阻塞轮询直至成功/失败/超时（两步链 20~180s 量级）。
    // Pre-condition: output_dir 与 name 非空；output_dir 不存在会自动创建。
    RigResult GenerateRigged(const RigConfig& config,
                             const std::string& output_dir,
                             const std::string& name);

    // 供诊断/测试覆盖超时。request_timeout_s=JSON 单请求，download_timeout_s=GLB
    // 下载，poll_deadline_s=单次轮询总上限（两步各自用这个上限）。默认即可。
    void SetTimeouts(long request_timeout_s, long poll_deadline_s,
                     long download_timeout_s = -1);

private:
    std::string api_key_;
    std::string base_url_;
    long request_timeout_s_ = 60;
    long download_timeout_s_ = 300;
    long poll_deadline_s_ = 240;  // 两步链每步各 240s 上限，总量更久

    // rig 方式映射后的 model 版本串（biped→v1.0-20240301，其余→v2.5-20260210），
    // config.rig_model_version 非空则优先用用户显式值。
    std::string ResolveRigModel(const RigConfig& config) const;
    // rig_type → tripo 枚举字符串
    std::string RigTypeToString(RigType t) const;
    // spec → tripo 枚举字符串
    std::string SpecToString(RigSpec s) const;

    // 建 Auto Rig 提交 body（input = rig 输入 task_id/url）
    std::string BuildRigBody(const std::string& input,
                             const std::string& model,
                             const std::string& rig_type,
                             const std::string& spec) const;
};

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_SKELETON_SKELETON_RIG_H_

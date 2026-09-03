// JPOV gen3d — Tripo3D REST 客户端
//
// TripoClient 是 gen3d 工具链里「唯一直接发 HTTP / 解析 tripo3d 响应」的类
// （延续 gen3d_config.h 的供应商无关设计——config 只表达用户想生成什么，
//  本 client 内部把 Gen3dConfig 映射成 tripo3d V3 的请求字段）。
//
// 供应商无关原则：凡调用方需要的都是「生成一模型 → 落盘 .glb」这类通用语义，
// 不暴露 task_id / 轮询 / model_url 等 tripo3d 专属细节。换供应商（Meshy 等）
// 只需新增一个同接口 client，调用链不变。
//
// 协议（tripo3d V3，2026-09 现行，见 docs/jpov_gen3d_design.md）：
//   1) POST /v3/generation/text-to-model
//        header: Authorization: Bearer <api_key>   Content-Type: application/json
//        body:   { "prompt", "model": "Q3D-...", "texture": true, "pbr": true, ... }
//        → { "code":0, "data":{ "task_id":"task_xxx" } }
//   2) 轮询 GET /v3/tasks/{task_id}  每 ~2s，仅带 Authorization 头
//        → status: success 时 output.model_url（GLB 签名单）
//   3) 从 model_url 下载到 output_dir/<name>.glb（签名单，不带 Authorization）
//   V2 已于 2026-11-01 退役，只用 V3。

#ifndef JPOV_GEN3D_TRIPO_CLIENT_H_
#define JPOV_GEN3D_TRIPO_CLIENT_H_

#include <string>

#include "tools/jpov/gen3d/gen3d_config.h"

namespace jpov {
namespace gen3d {

// 一次 tripo3d 生成任务的结果（仅暴露调用方关心的通用语义）。
struct Gen3dResult {
    // 生成的 GLB 落盘绝对路径；失败时为空。
    std::string glb_path;
    // 失败原因（空 = 成功）。供上层打印，不吞错误。
    std::string error;
};

// Tripo3D V3 REST 客户端。
class TripoClient {
public:
    // api_key 从环境变量 TRIPO_API_KEY 读取（严禁硬编码/入仓库）。
    // base_url 缺省 = https://openapi.tripo3d.ai/v3（全球端点）。
    explicit TripoClient(const std::string& api_key,
                         const std::string& base_url = "https://openapi.tripo3d.ai/v3");

    // 文本 prompt → 生成静态 PBR 模型 → 下载 GLB 到
    // <output_dir>/<name>.glb。成功后 result.glb_path = 该文件；失败置 error。
    // 会阻塞轮询直至成功/失败/超时（任务周期一般 10~120s）。
    // Pre-condition: output_dir 与 name 均非空；output_dir 不存在会自动创建。
    Gen3dResult GenerateTextToModel(const Gen3dConfig& config,
                                    const std::string& output_dir,
                                    const std::string& name);

    // 供调用方测试/诊断用的超时覆盖；仅改变行为不改变接口稳定语义。
    // 参数: request_timeout_s（JSON API 单请求）、download_timeout_s（GLB 下载，
    //     通常应大于 request）、poll_deadline_s（轮询总上限）。默认即可。
    void SetTimeouts(long request_timeout_s, long poll_deadline_s,
                     long download_timeout_s = -1);

private:
    std::string api_key_;
    std::string base_url_;
    long request_timeout_s_ = 60;    // JSON API 单请求超时（提交/轮询）
    long download_timeout_s_ = 300;  // GLB 下载单独更长超时（5 分钟过期前够下完大模型）
    long poll_deadline_s_ = 180;     // 总轮询时长上限

    // 从签名单 URL 下载到本地文件（GLB 可能较大，不落内存，边下边写盘）。
    // 下载不带 Authorization 头（tripo 签名 URL 已授权）；HTTP 层错误置 out_error。
    bool DownloadToFile(const std::string& url,
                        const std::string& dst_path,
                        std::string* out_error /*output*/) const;

    // 建 JSON 请求体（映射 Gen3dConfig → tripo3d 字段）。
    std::string BuildTextToModelBody(const Gen3dConfig& config) const;

    // 从响应解析 task_id；失败置 out_error。
    std::string ParseTaskId(const std::string& resp_body,
                            std::string* out_error /*output*/) const;

    // 从 todo 响应解析 model_url；失败返回空串并置 out_error。
    std::string ParseModelUrl(const std::string& resp_body,
                              std::string* out_error /*output*/) const;
};

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_TRIPO_CLIENT_H_

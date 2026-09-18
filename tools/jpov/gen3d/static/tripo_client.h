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
//
// 【按图出模】（2026-09-18 新增，见 docs/jpov_gen3d_image_input_design.md）：
//   图像输入需先上传取 file_token：
//     POST /v3/files   multipart/form-data 字段名 file → { "data":{ "file_token":"file_xxx" } }
//   再按 input_mode 提交：
//     kSingleImage → POST /v3/generation/image-to-model   body { "input": <token|URL>, ... }
//     kMultiview   → POST /v3/generation/multiview-to-model
//                    body { "inputs": { "front": <t>, "left": <t>, "back": <t>, "right": <t> }, ... }
//   视图顺序约定：front / left / back / right（Tripo 要求 front 不可省，至少 2 张）。
//   注：multiview 的 `inputs` 用 view-key 形式（推荐形式，顺序无关，服务端归一化）。

#ifndef JPOV_GEN3D_TRIPO_CLIENT_H_
#define JPOV_GEN3D_TRIPO_CLIENT_H_

#include <string>
#include <vector>

#include "tools/jpov/gen3d/static/gen3d_config.h"

namespace jpov {
namespace gen3d {

// 一次 tripo3d 生成任务的结果（仅暴露调用方关心的通用语义）。
struct Gen3dResult {
    // 生成的 GLB 落盘绝对路径；失败时为空。
    // （kImageToMultiview 模式不产 GLB，此字段为空。）
    std::string glb_path;
    // kImageToMultiview 模式产出的 4 张参考图落盘绝对路径，顺序
    // [front, left, back, right]。其他模式为空。
    std::vector<std::string> view_image_paths;
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

    // 统一生成入口：按 config.input_mode 分发到 文本 / 单图 / 多视图 三条路。
    //   kText        → text-to-model
    //   kSingleImage → 上传 input_image_paths[0] → image-to-model
    //   kMultiview   → 上传 input_image_paths[0..n)（顺序 [front,left,back,right]）
    //                  → multiview-to-model
    // 生成后统一下载 GLB 到 <output_dir>/<name>.glb。
    // 会阻塞轮询直至成功/失败/超时。
    // Pre-condition: output_dir 与 name 均非空；output_dir 不存在会自动创建；
    //   图像模式下 input_image_paths 数量须与 input_mode 匹配（见 CHECK）。
    // 注意：本方法**不做**图片格式审查（审查是调用方的职责，见 image_input.h）——
    //   保持"client 只管协议"的单一职责，且审查需在无 API key 时也能跑。
    Gen3dResult Generate(const Gen3dConfig& config,
                         const std::string& output_dir,
                         const std::string& name);

    // 【兼容封装】文本 prompt 专用入口，内部直接转 Generate。
    // 新代码请直接用 Generate + Gen3dConfig::input_mode；本方法保留只为
    // 让既有调用点（gen3d_cmd）语义不变。
    Gen3dResult GenerateTextToModel(const Gen3dConfig& config,
                                    const std::string& output_dir,
                                    const std::string& name);

    // 供调用方测试/诊断用的超时覆盖；仅改变行为不改变接口稳定语义。
    // 参数: request_timeout_s（JSON API 单请求）、download_timeout_s（GLB 下载，
    //     通常应大于 request）、poll_deadline_s（轮询总上限）。默认即可。
    void SetTimeouts(long request_timeout_s, long poll_deadline_s,
                     long download_timeout_s = -1);

    // 【断点续取】拿已知 task_id 直接轮询 + 下载产物，**不重新提交任务**。
    // 用途：上一轮客户端超时退出、但服务端任务实际已完成时，用 task_id 把
    //   产物捞回来，避免重复提交白烧 credit。
    // input_mode 决定产物形态：kImageToMultiview 产 4 张视图图；其余产 GLB。
    Gen3dResult ResumeByTaskId(Gen3dInputMode input_mode,
                              const std::string& task_id,
                              const std::string& output_dir,
                              const std::string& name);

private:
    std::string api_key_;
    std::string base_url_;
    long request_timeout_s_ = 60;    // JSON API 单请求超时（提交/轮询）
    long download_timeout_s_ = 300;  // GLB 下载单独更长超时（5 分钟过期前够下完大模型）
    // 轮询总上限。multiview/带贴图任务实测可跑 3 分钟以上（见 2026-09-18 wallet
    // 实测：任务 success 但客户端在 180s 就超时退出，产物没下载 → credit 白烧）。
    // 故放宽到 15 分钟，覆盖绝大多数任务；真正超长任务仍可 RescueByTaskId 续取。
    long poll_deadline_s_ = 900;

    // 从签名单 URL 下载到本地文件（GLB 可能较大，不落内存，边下边写盘）。
    // 下载不带 Authorization 头（tripo 签名 URL 已授权）；HTTP 层错误置 out_error。
    bool DownloadToFile(const std::string& url,
                        const std::string& dst_path,
                        std::string* out_error /*output*/) const;

    // 把 4 个视图 URL 逐张下载落盘（[front,left,back,right]）。
    // 由 GenerateImageToMultiview 与 ResumeByTaskId 共用，避免下载逻辑分叉。
    Gen3dResult DownloadMultiviewImages(const std::vector<std::string>& urls,
                                        const std::string& output_dir,
                                        const std::string& name) const;

    // 建 JSON 请求体（映射 Gen3dConfig → tripo3d 字段）。
    // 三条路各自一个 builder，共享同一套"固定管线约束"（恒 PBR/贴图/UV/三角面）。
    std::string BuildTextToModelBody(const Gen3dConfig& config) const;
    std::string BuildImageToModelBody(const Gen3dConfig& config,
                                      const std::string& file_token) const;
    std::string BuildMultiviewToModelBody(
        const Gen3dConfig& config,
        const std::vector<std::string>& file_tokens) const;

    // image-to-multiview（单图 → 4 视图参考图）。无 prompt / 无几何参数。
    std::string BuildImageToMultiviewBody(const Gen3dConfig& config,
                                         const std::string& file_token) const;

    // kImageToMultiview 主流程（产出 4 张图，非 GLB）。
    Gen3dResult GenerateImageToMultiview(const Gen3dConfig& config,
                                         const std::string& output_dir,
                                         const std::string& name);

    // 上传本地图片 → file_token。失败返回空串并置 out_error。
    // Pre-condition: path 非空且为可读的常规文件。
    std::string UploadFile(const std::string& path,
                           std::string* out_error /*output*/) const;

    // 轮询 task_url 直到 success/failed/canceled 或超时；成功后下载 GLB 到
    // dst_path。三条生成路共用的尾部流程（避免三份重复的轮询+下载代码）。
    // task_id 仅用于日志与错误信息。
    Gen3dResult PollAndDownload(const std::string& task_id,
                                const std::string& dst_path,
                                const std::string& context) const;

    // 从响应解析 task_id；失败置 out_error。
    std::string ParseTaskId(const std::string& resp_body,
                            std::string* out_error /*output*/) const;

    // 轮询 image-to-multiview 任务，从输出解析 4 张视图 URL。
    // 成功置 out_urls=[front,left,back,right]（缺的视图为空串）。
    bool PollMultiviewUrls(const std::string& task_id,
                           std::vector<std::string>* out_urls /*output*/,
                           std::string* out_error /*output*/) const;

    // 从 todo 响应解析 model_url；失败返回空串并置 out_error。
    std::string ParseModelUrl(const std::string& resp_body,
                              std::string* out_error /*output*/) const;
};

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_TRIPO_CLIENT_H_

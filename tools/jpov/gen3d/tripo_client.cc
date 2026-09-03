// JPOV gen3d — Tripo3D REST 客户端实现（见 tripo_client.h 协议说明）
#include "tools/jpov/gen3d/tripo_client.h"

#include <cerrno>
#include <chrono>  // NOLINT(build/c++11) 轮询计时
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>  // NOLINT(build/c++11) 轮询 sleep
#include <sys/stat.h>
#include <curl/curl.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>

namespace jpov {
namespace gen3d {
namespace {

using json = nlohmann::json;

// Tripo V3 低模（P1）当前稳定版本串。JPOV 主打游戏/静态 PBR 低模场景，
// 默认走 P1；高模（H 系列）暂未纳入本 client（如需可扩展 H3 分支）。
// 真实 API 可能需要随 Tripo 升级调版本——升版改这里即可。
constexpr char kLowPolyModel[] = "P1-20260311";

// 轮询节奏：每次间隔（ms）。
constexpr int kPollIntervalMs = 2 * 1000;

// libcurl 写回调目标：把响应体累积到 std::string*。
struct CurlWriteCtx {
    void* userdata;   // 指向 std::string*（内存模式）或 FILE*（文件模式）
    bool to_file;
};

// curl easy 写回调：内存模式追加到 std::string，文件模式写入 FILE*。
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (ctx->to_file) {
        auto* f = static_cast<FILE*>(ctx->userdata);
        return std::fwrite(ptr, 1, total, f);
    }
    auto* s = static_cast<std::string*>(ctx->userdata);
    s->append(ptr, total);
    return total;
}

// 用 libcurl 发一次请求并取回 body/状态。供提交/轮询与 DownloadToFile 复用。
//   - url / method：目标
//   - body：POST 体；method != "POST" 忽略
//   - auth_header：true 时加 `Authorization: Bearer <api_key>`
//   - to_file + file：false 时累积到 resp_body；true 时写到 FILE*
// 返回 curl 的错误码（CURLE_OK=0 表示传输层成功），HTTP 状态码写 out_status。
int CurlPerform(const std::string& api_key,
                const std::string& method,
                const std::string& url,
                const std::string& content_type,
                const std::string& body,
                bool auth_header,
                bool to_file,
                FILE* file,
                long request_timeout_s,
                std::string* resp_body /*output*/,
                bool* out_status_ok /*output*/) {
    CHECK_NOTNULL(resp_body);
    CHECK_NOTNULL(out_status_ok);

    CURL* curl = curl_easy_init();
    CHECK(curl != nullptr) << "curl_easy_init 失败";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    struct curl_slist* headers = nullptr;
    if (!content_type.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    }
    if (auth_header) {
        headers = curl_slist_append(
            headers, ("Authorization: Bearer " + api_key).c_str());
    }
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
        }
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        LOG(FATAL) << "不支持的 HTTP method: " << method;
    }

    CurlWriteCtx ctx;
    ctx.to_file = to_file;
    ctx.userdata = to_file ? static_cast<void*>(file)
                           : static_cast<void*>(resp_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    *out_status_ok = false;
    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) {
        // 传输层失败（DNS/连接/超时/TLS...）：把可读错误追加到 body，便于诊断。
        if (resp_body->empty()) {
            resp_body->append(curl_easy_strerror(rc));
        } else {
            resp_body->append(" (");
            resp_body->append(curl_easy_strerror(rc));
            resp_body->append(")");
        }
    }
    *out_status_ok = (rc == CURLE_OK && http_code >= 200 && http_code < 300);

    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : 1;
}

// 绝对路径从 output_dir + name + ".glb" 组装，并确保 output_dir 存在（mkdir -p）。
std::string JoinGlbPath(const std::string& output_dir, const std::string& name) {
    CHECK(!output_dir.empty() && !name.empty());
    if (::mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG(FATAL) << "无法创建输出目录: " << output_dir
                   << " (errno=" << errno << ")";
    }
    std::string p = output_dir;
    if (!p.empty() && p[p.size() - 1] != '/') {
        p.push_back('/');
    }
    return p + name + ".glb";
}

// 简单秒级时间戳（用于轮询截止判断）。
struct Clock {
    static long NowEpochS() {
        using namespace std::chrono;  // NOLINT 局部有限使用
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }
};

}  // namespace

TripoClient::TripoClient(const std::string& api_key,
                         const std::string& base_url)
    : api_key_(api_key), base_url_(base_url) {}

void TripoClient::SetTimeouts(long request_timeout_s, long poll_deadline_s,
                              long download_timeout_s) {
    request_timeout_s_ = request_timeout_s;
    poll_deadline_s_ = poll_deadline_s;
    if (download_timeout_s > 0) {
        download_timeout_s_ = download_timeout_s;
    }
}

Gen3dResult TripoClient::GenerateTextToModel(const Gen3dConfig& config,
                                             const std::string& output_dir,
                                             const std::string& name) {
    Gen3dResult result;
    if (api_key_.empty()) {
        result.error = "API key 为空（TripoClient 需 api_key，tripo_cmd 用 TRIPO_API_KEY）";
        return result;
    }

    // ---- 1) 提交任务 ----
    const std::string submit_url = base_url_ + "/generation/text-to-model";
    const std::string body = BuildTextToModelBody(config);
    std::string submit_body;
    bool submit_ok = false;
    CurlPerform(api_key_, "POST", submit_url, "application/json", body,
                /*auth_header*/ true, /*to_file*/ false, nullptr,
                request_timeout_s_, &submit_body, &submit_ok);
    if (!submit_ok) {
        if (submit_body.empty()) {
            result.error = "提交任务失败：无响应体（网络不通或超时），请检查网络/TRIpo 端点可达";
        } else {
            result.error = "提交任务失败 (HTTP): " + submit_body;
        }
        return result;
    }

    std::string task_id;
    {
        std::string parse_error;
        task_id = ParseTaskId(submit_body, &parse_error);
        if (task_id.empty()) {
            result.error = parse_error;
            return result;
        }
    }
    LOG(INFO) << "tripo 任务已提交: task_id=" << task_id;

    // ---- 2) 轮询 ----
    const std::string task_url = base_url_ + "/tasks/" + task_id;
    const long deadline = Clock::NowEpochS() + poll_deadline_s_;
    std::string final_error = "任务超时（>"
        + std::to_string(poll_deadline_s_) + "s）仍未完成: task_id=" + task_id;

    while (Clock::NowEpochS() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        std::string poll_body;
        bool poll_ok = false;
        CurlPerform(api_key_, "GET", task_url, "", "", /*auth_header*/ true,
                    /*to_file*/ false, nullptr, request_timeout_s_,
                    &poll_body, &poll_ok);
        if (!poll_ok) {
            final_error = "轮询请求失败: " + poll_body;
            continue;
        }
        json j;
        try {
            j = json::parse(poll_body);
        } catch (const json::parse_error&) {
            final_error = "轮询响应非合法 JSON";
            continue;
        }
        if (!j.contains("data") || !j["data"].is_object() ||
            !j["data"].contains("status")) {
            final_error = "轮询响应缺少 data.status";
            continue;
        }
        const std::string status = j["data"]["status"];
        LOG(INFO) << "tripo 任务 " << task_id << " 状态: " << status
                  << " (已用时 " << (Clock::NowEpochS() - deadline + poll_deadline_s_)
                  << "s)";
        if (status == "success") {
            std::string poll_error;
            const std::string model_url = ParseModelUrl(poll_body, &poll_error);
            if (model_url.empty()) {
                result.error = "任务成功但解析 model_url 失败: " + poll_error;
                return result;
            }
            // ---- 3) 下载 ----
            const std::string glb_path = JoinGlbPath(output_dir, name);
            std::string dl_error;
            if (!DownloadToFile(model_url, glb_path, &dl_error)) {
                result.error = "下载 GLB 失败: " + dl_error;
                return result;
            }
            result.glb_path = glb_path;
            return result;
        }
        if (status == "failed" || status == "canceled") {
            result.error = "tripo 任务" + status + ": " + poll_body;
            return result;
        }
        // 其他（processing/queued/...）继续轮询。
    }
    result.error = final_error;
    return result;
}


bool TripoClient::DownloadToFile(const std::string& url,
                                 const std::string& dst_path,
                                 std::string* out_error /*output*/) const {
    CHECK_NOTNULL(out_error);
    FILE* f = std::fopen(dst_path.c_str(), "wb");
    if (f == nullptr) {
        *out_error = "无法写入文件: " + dst_path + " (errno="
            + std::to_string(errno) + ")";
        return false;
    }
    std::string curl_diag;   // 保存 CurlPerform 传入的可读传输诊断
    bool ok = false;
    // GLB 下载用独立更长超时（download_timeout_s_ 默认 300s）；模型即便较大
    // 也应在 5 分钟签名过期前下完。download URL 多为 CDN 大文件慢链路。
    CurlPerform(api_key_, "GET", url, "", "",
                /*auth_header*/ false, /*to_file*/ true, f,
                download_timeout_s_, &curl_diag, &ok);
    std::fclose(f);
    if (!ok) {
        *out_error = "下载失败: " + curl_diag;
        std::remove(dst_path.c_str());
        return false;
    }
    return true;
}

std::string TripoClient::BuildTextToModelBody(const Gen3dConfig& config) const {
    json j;
    j["prompt"] = config.prompt;
    if (!config.negative_prompt.empty()) {
        j["negative_prompt"] = config.negative_prompt;
    }
    // 固定管线约束：恒 PBR + 恒贴图 + 恒有 UV（gen3d_config.h 顶部约定）。
    j["texture"] = true;
    j["pbr"] = true;
    j["export_uv"] = true;
    j["texture_quality"] = "standard";
    // 尺寸语义：auto_size 对应 config.real_size（true=按米真实尺寸）。
    j["auto_size"] = config.real_size;
    // 模型档位与面数：low_poly → P1 + face_limit；高模暂用自适应（本次主打低模）。
    if (config.low_poly) {
        j["model"] = kLowPolyModel;
        // max_triangles：>0 时发 face_limit，并 clamp 到 P1 支持范围 50~20000。
        // -1 = 不限制（供应商自适应拓扑）。防御式：即使调用方传超范围也合法。
        if (config.max_triangles > 0) {
            const int kP1FaceMin = 50;
            const int kP1FaceMax = 20000;
            int face_limit = config.max_triangles;
            if (face_limit < kP1FaceMin) face_limit = kP1FaceMin;
            if (face_limit > kP1FaceMax) face_limit = kP1FaceMax;
            j["face_limit"] = face_limit;
        }
    } else {
        // 高模走 H 系列；本次 client 暂未核实 H 当前版本串，交由调用方在实测
        // 阶段确认为 safest —— 默认也发 low_poly=false 时用 P1 取顶上限。
        LOG(WARNING) << "high_poly(low_poly=false) 暂未映射独立 H 档，退回 P1 自适应";
        j["model"] = kLowPolyModel;
    }
    return j.dump();
}

std::string TripoClient::ParseTaskId(const std::string& resp_body,
                                     std::string* out_error /*output*/) const {
    CHECK_NOTNULL(out_error);
    try {
        const json j = json::parse(resp_body);
        if (j.contains("code") && j["code"] != 0) {
            *out_error = "tripo 返回错误 code=" + j["code"].dump()
                + " msg=" + (j.contains("msg") ? j["msg"].dump() : "(无)");
            return "";
        }
        if (!j.contains("data") || !j["data"].is_object() ||
            !j["data"].contains("task_id")) {
            *out_error = "响应缺少 data.task_id: " + resp_body;
            return "";
        }
        return j["data"]["task_id"].get<std::string>();
    } catch (const json::exception& e) {
        *out_error = "JSON 解析失败: " + std::string(e.what());
        return "";
    }
}

std::string TripoClient::ParseModelUrl(const std::string& resp_body,
                                       std::string* out_error /*output*/) const {
    CHECK_NOTNULL(out_error);
    try {
        const json j = json::parse(resp_body);
        if (!j.contains("data") || !j["data"].is_object() ||
            !j["data"].contains("output") || !j["data"]["output"].is_object()) {
            *out_error = "任务结果缺少 data.output: " + resp_body;
            return "";
        }
        const json& out = j["data"]["output"];
        if (!out.contains("model_url") || !out["model_url"].is_string()) {
            *out_error = "任务结果缺少 output.model_url: " + resp_body;
            return "";
        }
        return out["model_url"].get<std::string>();
    } catch (const json::exception& e) {
        *out_error = "JSON 解析失败: " + std::string(e.what());
        return "";
    }
}

}  // namespace gen3d
}  // namespace jpov

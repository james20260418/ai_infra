// JPOV gen3d — Tripo3D REST 客户端实现（见 tripo_client.h 协议说明）
#include "tools/jpov/gen3d/static/tripo_client.h"

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

// 绝对路径从 output_dir + name + ext 组装，并确保 output_dir 存在（mkdir -p）。
// 供产图模式（image-to-multiview 的 _front.png 等）使用。
std::string JoinNamedPath(const std::string& output_dir,
                         const std::string& name,
                         const std::string& ext) {
    CHECK(!output_dir.empty() && !name.empty());
    if (::mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG(FATAL) << "无法创建输出目录: " << output_dir
                   << " (errno=" << errno << ")";
    }
    std::string p = output_dir;
    if (!p.empty() && p[p.size() - 1] != '/') {
        p.push_back('/');
    }
    return p + name + ext;
}

// 简单秒级时间戳（用于轮询截止判断）。
struct Clock {
    static long NowEpochS() {
        using namespace std::chrono;  // NOLINT 局部有限使用
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }
};

// 三条生成路共享的"固定管线约束"：恒 PBR + 恒贴图 + 恒 UV + 恒标准贴图质量。
// 抽出来是为了让 image / multiview 两条新路与原 text 路径**永不发生参数分叉**
// （历史上 generator 与 test 分叉过，见 MEMORY 里 mrquad 教训）。
void ApplySharedPipelineConstraints(const Gen3dConfig& config, json* out) {
    CHECK_NOTNULL(out);
    (*out)["texture"] = true;
    (*out)["pbr"] = true;
    (*out)["export_uv"] = true;
    (*out)["texture_quality"] = "standard";
    (*out)["auto_size"] = config.real_size;

    // 模型档位与面数：low_poly → P1 + face_limit；高模暂用自适应。
    // 注：P1 面数上限 20000。
    (*out)["model"] = kLowPolyModel;
    if (config.low_poly && config.max_triangles > 0) {
        const int kP1FaceMin = 50;
        const int kP1FaceMax = 20000;
        int face_limit = config.max_triangles;
        if (face_limit < kP1FaceMin) {
            face_limit = kP1FaceMin;
        }
        if (face_limit > kP1FaceMax) {
            face_limit = kP1FaceMax;
        }
        (*out)["face_limit"] = face_limit;
    }
}

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
    // 薄封装：保留旧调用点（gen3d_cmd 等）不必改。语义即 kText 模式。
    // 若调用方传入了图像路径则与 kText 冲突，Generate 内部的 CHECK 会拦住。
    return Generate(config, output_dir, name);
}

Gen3dResult TripoClient::Generate(const Gen3dConfig& config,
                                  const std::string& output_dir,
                                  const std::string& name) {
    Gen3dResult result;
    if (api_key_.empty()) {
        result.error = "API key 为空（TripoClient 需 api_key，tripo_cmd 用 TRIPO_API_KEY）";
        return result;
    }
    if (output_dir.empty() || name.empty()) {
        result.error = "output_dir 与 name 均不能为空";
        return result;
    }

    // kImageToMultiview 是「产图」而非「产 3D」，尾部流程与其余三个模式不同，
    // 故单独分发（其余三个共用下面的提交 + PollAndDownload）。
    if (config.input_mode == Gen3dInputMode::kImageToMultiview) {
        return GenerateImageToMultiview(config, output_dir, name);
    }

    // ---- 分派：按输入方式选端点、组装请求体 ----
    std::string endpoint;
    std::string body;
    switch (config.input_mode) {
        case Gen3dInputMode::kText: {
            CHECK(config.input_image_paths.empty())
                << "kText 模式不应携带 input_image_paths";
            endpoint = "/generation/text-to-model";
            body = BuildTextToModelBody(config);
            break;
        }
        case Gen3dInputMode::kSingleImage: {
            CHECK_EQ(config.input_image_paths.size(), 1u)
                << "kSingleImage 模式需要恰好 1 张参考图";
            std::string up_err;
            const std::string token =
                UploadFile(config.input_image_paths[0], &up_err);
            if (token.empty()) {
                result.error = "上传参考图失败: " + up_err;
                return result;
            }
            LOG(INFO) << "参考图已上传: " << config.input_image_paths[0]
                      << " → " << token;
            endpoint = "/generation/image-to-model";
            body = BuildImageToModelBody(config, token);
            break;
        }
        case Gen3dInputMode::kMultiview: {
            const size_t n = config.input_image_paths.size();
            CHECK_GE(n, 2u) << "kMultiview 至少需要 2 张图（Tripo 要求）";
            CHECK_LE(n, 4u) << "kMultiview 最多 4 张图（front/left/back/right）";
            std::vector<std::string> tokens;
            tokens.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                std::string up_err;
                const std::string t = UploadFile(config.input_image_paths[i], &up_err);
                if (t.empty()) {
                    result.error = "上传视图失败: "
                        + config.input_image_paths[i] + ": " + up_err;
                    return result;
                }
                LOG(INFO) << "视图已上传: " << config.input_image_paths[i]
                          << " → " << t;
                tokens.push_back(t);
            }
            endpoint = "/generation/multiview-to-model";
            body = BuildMultiviewToModelBody(config, tokens);
            break;
        }
        case Gen3dInputMode::kImageToMultiview:
            LOG(FATAL) << "kImageToMultiview 应在函数开头单独分发";
        default:
            LOG(FATAL) << "未知 Gen3dInputMode";
    }

    // ---- 提交任务 ----
    const std::string submit_url = base_url_ + endpoint;
    std::string submit_body;
    bool submit_ok = false;
    CurlPerform(api_key_, "POST", submit_url, "application/json", body,
                /*auth_header*/ true, /*to_file*/ false, nullptr,
                request_timeout_s_, &submit_body, &submit_ok);
    if (!submit_ok) {
        if (submit_body.empty()) {
            result.error = "提交任务失败：无响应体（网络不通或超时），请检查网络/Tripo 端点可达";
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
    LOG(INFO) << "tripo 任务已提交: task_id=" << task_id
              << " (endpoint=" << endpoint << ")";

    // ---- 轮询 + 下载（三条路共用）----
    return PollAndDownload(task_id, JoinGlbPath(output_dir, name),
                           /*context*/ endpoint);
}

Gen3dResult TripoClient::PollAndDownload(const std::string& task_id,
                                         const std::string& dst_path,
                                         const std::string& context) const {
    Gen3dResult result;
    const std::string task_url = base_url_ + "/tasks/" + task_id;
    const long deadline = Clock::NowEpochS() + poll_deadline_s_;
    // 超时错误必须带 task_id：可拿它续取，不必重跑（重跑=白烧 credit）。
    // 见 ResumeByTaskId / --resume_task。
    std::string final_error = "任务超时（>"
        + std::to_string(poll_deadline_s_) + "s）仍未完成: task_id=" + task_id
        + "（服务端可能仍在跑，用 --resume_task " + task_id + " 续取，勿重跑）";

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
            std::string dl_error;
            if (!DownloadToFile(model_url, dst_path, &dl_error)) {
                result.error = "下载 GLB 失败: " + dl_error;
                return result;
            }
            result.glb_path = dst_path;
            return result;
        }
        if (status == "failed" || status == "canceled") {
            // 带 task_id：失败/取消的任务也要可追溯（用户可能拿它找 Tripo 支持）。
            result.error = "tripo 任务" + status + " (task_id=" + task_id
                + ", context=" + context + "): " + poll_body;
            return result;
        }
        // 其他（processing/queued/...）继续轮询。
    }
    result.error = final_error;
    return result;
}

// kImageToMultiview：单图 → 生成 4 视图参考图。
// 与另三条生成路的关键区别：① 端点不同；② **产出是 4 张图而不是 GLB**：
//   Tripo 返回的是 4 个签名 URL，我们需要逐个下载并落盘。
Gen3dResult TripoClient::GenerateImageToMultiview(const Gen3dConfig& config,
                                                  const std::string& output_dir,
                                                  const std::string& name) {
    Gen3dResult result;
    CHECK_EQ(config.input_image_paths.size(), 1u)
        << "kImageToMultiview 需要恰好 1 张输入图（产出才是 4 视图）";

    // ---- 1) 上传输入图 ----
    std::string up_err;
    const std::string token = UploadFile(config.input_image_paths[0], &up_err);
    if (token.empty()) {
        result.error = "上传参考图失败: " + up_err;
        return result;
    }
    LOG(INFO) << "参考图已上传: " << config.input_image_paths[0]
              << " → " << token;

    // ---- 2) 提交 image-to-multiview ----
    const std::string submit_url = base_url_ + "/generation/image-to-multiview";
    const std::string body = BuildImageToMultiviewBody(config, token);
    std::string submit_body;
    bool submit_ok = false;
    CurlPerform(api_key_, "POST", submit_url, "application/json", body,
                /*auth_header*/ true, /*to_file*/ false, nullptr,
                request_timeout_s_, &submit_body, &submit_ok);
    if (!submit_ok) {
        result.error = submit_body.empty()
            ? "提交 image-to-multiview 失败：无响应体（网络不通或超时）"
            : "提交 image-to-multiview 失败 (HTTP): " + submit_body;
        return result;
    }
    std::string parse_error;
    const std::string task_id = ParseTaskId(submit_body, &parse_error);
    if (task_id.empty()) {
        result.error = parse_error;
        return result;
    }
    LOG(INFO) << "image-to-multiview 任务已提交: task_id=" << task_id;

    // ---- 3) 轮询取 4 个视图 URL ----
    std::vector<std::string> urls;
    std::string poll_err;
    if (!PollMultiviewUrls(task_id, &urls, &poll_err)) {
        result.error = poll_err;
        return result;
    }

    // ---- 4) 逐张下载落盘 ----
    return DownloadMultiviewImages(urls, output_dir, name);
}

// 把 4 个视图 URL 逐张下载落盘（[front,left,back,right]）。
// 独立出来供 GenerateImageToMultiview 与 ResumeByTaskId 共用。
Gen3dResult TripoClient::DownloadMultiviewImages(
    const std::vector<std::string>& urls,
    const std::string& output_dir,
    const std::string& name) const {
    Gen3dResult result;
    constexpr char kViewNames[4][8] = {"front", "left", "back", "right"};
    if (urls.size() != 4) {
        result.error = "image-to-multiview 返回的视图数不是 4（实际 "
            + std::to_string(urls.size()) + "）";
        return result;
    }
    for (size_t i = 0; i < urls.size(); ++i) {
        if (urls[i].empty()) {
            // 允许缺视图（Tripo 允许），但记录警告并跳过落盘。
            LOG(WARNING) << "image-to-multiview 的 " << kViewNames[i]
                         << " 视图为空，跳过落盘";
            continue;
        }
        const std::string png_path = JoinNamedPath(
            output_dir, name, std::string("_") + kViewNames[i] + ".png");
        std::string dl_err;
        if (!DownloadToFile(urls[i], png_path, &dl_err)) {
            result.error = std::string("下载 ") + kViewNames[i]
                + " 视图失败: " + dl_err;
            return result;
        }
        LOG(INFO) << "已落盘 " << kViewNames[i] << " 视图: " << png_path;
        result.view_image_paths.push_back(png_path);
    }
    if (result.view_image_paths.empty()) {
        result.error = "image-to-multiview 未产出任何可用视图";
    }
    return result;
}

// 【断点续取】已知 task_id 时直接轮询 + 下载，**不重新提交**。
// 动机：客户端超时退出而服务端任务实际已完成时，重跑会白烧 credit；
//   用 task_id 把已产出的结果捞回来才是正确做法（见 2026-09-18 wallet 实测）。
Gen3dResult TripoClient::ResumeByTaskId(Gen3dInputMode input_mode,
                                        const std::string& task_id,
                                        const std::string& output_dir,
                                        const std::string& name) {
    CHECK(!task_id.empty()) << "--resume_task 需要非空 task_id";
    LOG(INFO) << "断点续取: task_id=" << task_id
              << " (不重新提交任务，直接轮询+下载)";

    if (input_mode == Gen3dInputMode::kImageToMultiview) {
        // 产图模式：轮询拿 4 个视图 URL，再逐张落盘。
        std::vector<std::string> urls;
        std::string poll_err;
        if (!PollMultiviewUrls(task_id, &urls, &poll_err)) {
            Gen3dResult r;
            r.error = poll_err;
            return r;
        }
        return DownloadMultiviewImages(urls, output_dir, name);
    }

    // 产模型模式：轮询 success 后拿 model_url 并下载 GLB。
    return PollAndDownload(task_id, JoinGlbPath(output_dir, name),
                           /*context*/ "resume");
}

bool TripoClient::PollMultiviewUrls(const std::string& task_id,
                                    std::vector<std::string>* out_urls,
                                    std::string* out_error) const {
    CHECK_NOTNULL(out_urls);
    CHECK_NOTNULL(out_error);
    const std::string task_url = base_url_ + "/tasks/" + task_id;
    const long deadline = Clock::NowEpochS() + poll_deadline_s_;
    std::string final_error = "任务超时（>"
        + std::to_string(poll_deadline_s_) + "s）仍未完成: task_id=" + task_id
        + "（服务端可能仍在跑，用 --resume_task " + task_id + " 续取，勿重跑）";

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
        LOG(INFO) << "image-to-multiview " << task_id << " 状态: " << status;
        if (status == "success") {
            // 官方输出字段：front_view_url / left_view_url / back_view_url /
            // right_view_url（见 docs generation-image-to-multiview）。
            constexpr char kUrlKeys[4][16] = {
                "front_view_url", "left_view_url",
                "back_view_url", "right_view_url"};
            const json& out = j["data"]["output"];
            for (int i = 0; i < 4; ++i) {
                std::string u;
                if (out.contains(kUrlKeys[i]) && out[kUrlKeys[i]].is_string()) {
                    u = out[kUrlKeys[i]].get<std::string>();
                }
                out_urls->push_back(u);
            }
            return true;
        }
        if (status == "failed" || status == "canceled") {
            *out_error = "image-to-multiview 任务" + status + ": " + poll_body;
            return false;
        }
    }
    *out_error = final_error;
    return false;
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
    if (config.prompt.empty()) {
        LOG(WARNING) << "text-to-model 的 prompt 为空——Tripo 大概率返回错误";
    }
    if (!config.low_poly) {
        // 高模（H 系列）版本串尚未在本 client 核准，先退回 P1 并告警。
        LOG(WARNING) << "high_poly(low_poly=false) 暂未映射独立 H 档，退回 P1 自适应";
    }
    ApplySharedPipelineConstraints(config, &j);
    return j.dump();
}

std::string TripoClient::BuildImageToModelBody(
    const Gen3dConfig& config, const std::string& file_token) const {
    CHECK(!file_token.empty()) << "BuildImageToModelBody 需要非空 file_token";
    json j;
    j["input"] = file_token;
    // orientation：align_image 需 texture=true（固定管线已是 true，故有效）。
    j["orientation"] = config.align_to_image ? "align_image" : "default";
    ApplySharedPipelineConstraints(config, &j);
    return j.dump();
}

std::string TripoClient::BuildMultiviewToModelBody(
    const Gen3dConfig& config,
    const std::vector<std::string>& file_tokens) const {
    CHECK_GE(file_tokens.size(), 2u);
    CHECK_LE(file_tokens.size(), 4u);
    // view-key 形式（Tripo 推荐）：顺序无关，服务端归一化到 [front,left,back,right]。
    // 位置即视图语义：index 0..3 = front / left / back / right。
    constexpr char kViewKeys[4][8] = {"front", "left", "back", "right"};
    json inputs = json::array();
    for (size_t i = 0; i < file_tokens.size(); ++i) {
        CHECK(!file_tokens[i].empty())
            << "multiview 第 " << i << " 个 file_token 为空";
        json item;
        item[kViewKeys[i]] = file_tokens[i];
        inputs.push_back(item);
    }
    json j;
    j["inputs"] = inputs;
    j["orientation"] = config.align_to_image ? "align_image" : "default";
    ApplySharedPipelineConstraints(config, &j);
    return j.dump();
}

std::string TripoClient::UploadFile(const std::string& path,
                                    std::string* out_error /*output*/) const {
    CHECK_NOTNULL(out_error);
    CHECK(!path.empty()) << "UploadFile 需要非空路径";

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        *out_error = "curl_easy_init 失败";
        return "";
    }
    // multipart/form-data：字段名固定为 file（Tripo 约定）。
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, path.c_str());

    const std::string url = base_url_ + "/files";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout_s_);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, ("Authorization: Bearer " + api_key_).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string resp;
    CurlWriteCtx ctx;
    ctx.to_file = false;
    ctx.userdata = static_cast<void*>(&resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        *out_error = "上传传输失败: " + std::string(curl_easy_strerror(rc));
        return "";
    }
    if (http_code < 200 || http_code >= 300) {
        *out_error = "上传返回 HTTP " + std::to_string(http_code) + ": " + resp;
        return "";
    }
    // 响应形如 { "code":0, "data":{ "file_token":"file_xxx" } }
    try {
        const json j = json::parse(resp);
        if (!j.contains("data") || !j["data"].is_object() ||
            !j["data"].contains("file_token")) {
            *out_error = "上传响应缺少 data.file_token: " + resp;
            return "";
        }
        return j["data"]["file_token"].get<std::string>();
    } catch (const json::exception& e) {
        *out_error = "上传响应 JSON 解析失败: " + std::string(e.what());
        return "";
    }
}

std::string TripoClient::BuildImageToMultiviewBody(
    const Gen3dConfig& config, const std::string& file_token) const {
    CHECK(!file_token.empty()) << "BuildImageToMultiviewBody 需要非空 file_token";
    json j;
    j["input"] = file_token;
    // 注：image-to-multiview **不接受 prompt**（产出为固定的 4 个正交视图），
    // 也不接受 face_limit / texture 等几何参数（它产的是图不是网格）。
    // 固定管线约束在此不适用。唯一透传的可选项是 align_to_image 语义无对应，
    // 故无需其他字段。
    (void)config;
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

// JPOV gen3d/skeleton — TripoRigClient 实现（见 skeleton_rig.h 契约说明）
//
// 两步 task 链 + 独立 HTTP/轮询/下载（curl 模式参考 static tripo_client.cc，
// 但为不侵入已稳定的 static，此处自实现一套等价的 HTTP 基础设施）。
#include "tools/jpov/gen3d/skeleton/skeleton_rig.h"

#include <cerrno>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <sys/stat.h>
#include <curl/curl.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>

namespace jpov {
namespace gen3d {
namespace {

using json = nlohmann::json;

// tripo 生成端点相对 base_url 的路径。
constexpr char kTextToModelPath[] = "/generation/text-to-model";
constexpr char kAutoRigPath[] = "/animations/rig";

// 静态生成缺省低模版本（text-to-model 无论 rig 与否，先生成一个可被 rig 的静态
// 人形——这里跟随 static 的 P1 低模默认足够)。rig 前的静态网格只需具备可识别
// 的人形拓扑即可被接骨，不必高精细；低模更便宜且够 rig。
constexpr char kStaticModel[] = "P1-20260311";

// 轮询间隔。
constexpr int kPollIntervalMs = 2 * 1000;

// curl 写回调环境：内存累积到 std::string，或写 FILE*。
struct WriteCtx {
    void* userdata;
    bool to_file;
};
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* ctx = static_cast<WriteCtx*>(userdata);
    if (ctx->to_file) {
        return std::fwrite(ptr, 1, total, static_cast<FILE*>(ctx->userdata));
    }
    static_cast<std::string*>(ctx->userdata)->append(ptr, total);
    return total;
}

// 发一次 HTTP 并取回 body / 是否 2xx。auth_header=true 加 Bearer 头。
// to_file=true 时响应写 FILE*（下载用）。POST/GET 二选一。
int CurlPerform(const std::string& api_key,
                const std::string& method,
                const std::string& url,
                const std::string& body,
                bool auth_header,
                bool to_file,
                FILE* file,
                long timeout_s,
                std::string* resp,
                bool* out_ok) {
    CURL* curl = curl_easy_init();
    CHECK(curl != nullptr) << "curl_easy_init 失败";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    struct curl_slist* hdrs = nullptr;
    // auth_header=true => 发 JSON API（提交/轮询），需 content-type + Bearer。
    // auth_header=false => 下载（签名 URL 已授权），不加任何头。
    if (auth_header) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(
            hdrs, ("Authorization: Bearer " + api_key).c_str());
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
        }
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    WriteCtx ctx;
    ctx.to_file = to_file;
    ctx.userdata = to_file ? static_cast<void*>(file)
                           : static_cast<void*>(resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    *out_ok = false;
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc != CURLE_OK) {
        if (resp->empty()) resp->append(curl_easy_strerror(rc));
        else resp->append(" (" + std::string(curl_easy_strerror(rc)) + ")");
    }
    *out_ok = (rc == CURLE_OK && code >= 200 && code < 300);

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : 1;
}

// output_dir + name + ".glb" 绝对路径，并 mkdir -p output_dir。
std::string JoinGlbPath(const std::string& output_dir,
                        const std::string& name) {
    CHECK(!output_dir.empty() && !name.empty());
    if (::mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG(FATAL) << "无法创建输出目录: " << output_dir
                   << " (errno=" << errno << ")";
    }
    std::string p = output_dir;
    if (!p.empty() && p[p.size() - 1] != '/') p.push_back('/');
    return p + name + ".glb";
}

// 秒级时间戳。
struct Clock {
    static long NowS() {
        using namespace std::chrono;  // NOLINT
        return duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
    }
};

// 从提交响应 body 提取 task_id；失败置 out_error 返回空。
std::string ParseTaskId(const std::string& body, std::string* out_error) {
    try {
        const json j = json::parse(body);
        if (j.contains("code") && j["code"] != 0) {
            *out_error = "tripo 返回错误 code=" + j["code"].dump()
                + " msg=" + (j.contains("msg") ? j["msg"].dump() : "(无)");
            return "";
        }
        if (!j.contains("data") || !j["data"].is_object() ||
            !j["data"].contains("task_id")) {
            *out_error = "响应缺少 data.task_id: " + body;
            return "";
        }
        return j["data"]["task_id"].get<std::string>();
    } catch (const json::exception& e) {
        *out_error = "JSON 解析失败: " + std::string(e.what());
        return "";
    }
}

// 轮询任务直到 success, 返回 output.model_url；失败/超时置 out_error 返回空。
// task_url 是 GET {base}/tasks/{id}。
std::string PollTask(const std::string& api_key,
                     const std::string& task_id,
                     const std::string& base_url,
                     long request_timeout_s,
                     long poll_deadline_s,
                     bool* ok_out,        // 任务层成功(拿到 model_url)
                     std::string* out_error /*output*/) {
    CHECK_NOTNULL(ok_out);
    CHECK_NOTNULL(out_error);
    *ok_out = false;
    const std::string task_url = base_url + "/tasks/" + task_id;
    const long deadline = Clock::NowS() + poll_deadline_s;
    std::string final_error =
        "任务超时(>" + std::to_string(poll_deadline_s) + "s): task_id=" + task_id;

    while (Clock::NowS() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        std::string poll_body;
        bool poll_ok = false;
        CurlPerform(api_key, "GET", task_url, /*body*/ "", /*auth*/ true,
                    /*to_file*/ false, nullptr, request_timeout_s,
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
        LOG(INFO) << "tripo task " << task_id << " status: " << status;
        if (status == "success") {
            const json& out = j["data"];
            if (!out.contains("output") || !out["output"].is_object()) {
                *out_error = "任务成功但缺少 output: " + poll_body;
                return "";
            }
            const json& o = out["output"];
            // rig / retarget 用 model_url；rig-check 用 riggable/rig_type(不用)。
            if (o.contains("model_url") && o["model_url"].is_string()) {
                *ok_out = true;
                return o["model_url"].get<std::string>();
            }
            *out_error = "任务成功但 output 无 model_url(非 GLB 任务?): " + poll_body;
            return "";
        }
        if (status == "failed" || status == "canceled") {
            *out_error = "tripo 任务" + status + ": " + poll_body;
            return "";
        }
        // processing/queued... 继续轮询。
    }
    *out_error = final_error;
    return "";
}

}  // namespace

TripoRigClient::TripoRigClient(const std::string& api_key,
                               const std::string& base_url)
    : api_key_(api_key), base_url_(base_url) {}

void TripoRigClient::SetTimeouts(long request_timeout_s,
                                 long poll_deadline_s,
                                 long download_timeout_s) {
    request_timeout_s_ = request_timeout_s;
    poll_deadline_s_ = poll_deadline_s;
    if (download_timeout_s > 0) download_timeout_s_ = download_timeout_s;
}

std::string TripoRigClient::RigTypeToString(RigType t) const {
    switch (t) {
        case RigType::kBiped:      return "biped";
        case RigType::kQuadruped:  return "quadruped";
        case RigType::kHexapod:    return "hexapod";
        case RigType::kOctopod:    return "octopod";
        case RigType::kAvian:      return "avian";
        case RigType::kSerpentine: return "serpentine";
        case RigType::kAquatic:    return "aquatic";
    }
    return "biped";
}

std::string TripoRigClient::SpecToString(RigSpec s) const {
    return s == RigSpec::kMixamo ? "mixamo" : "tripo";
}

std::string TripoRigClient::ResolveRigModel(const RigConfig& config) const {
    if (!config.rig_model_version.empty()) return config.rig_model_version;
    return config.rig_type == RigType::kBiped ? "v1.0-20240301"
                                              : "v2.5-20260210";
}

std::string TripoRigClient::BuildRigBody(const std::string& input,
                                         const std::string& model,
                                         const std::string& rig_type,
                                         const std::string& spec) const {
    json j;
    j["input"] = input;  // task_id 或 URL
    j["model"] = model;
    j["rig_type"] = rig_type;
    j["spec"] = spec;
    j["out_format"] = "glb";
    return j.dump();
}

RigResult TripoRigClient::GenerateRigged(const RigConfig& config,
                                         const std::string& output_dir,
                                         const std::string& name) {
    RigResult result;
    if (api_key_.empty()) {
        result.error = "API key 为空（TripoRigClient 需 api_key，三脚架脚本用 TRIPO_API_KEY）";
        return result;
    }

    // ---- 决定 Auto Rig 的 input ----
    std::string rig_input;               // task_id 或直链 URL
    std::string describe_for_log;
    if (config.input_mode == RigInputMode::kTextToModel) {
        // kTextToModel：先 text-to-model 生成一个静态模型，拿 task_id 再 rig。
        if (config.prompt.empty()) {
            result.error = "kTextToModel 模式需要 --prompt";
            return result;
        }
        // (a) 提交 text-to-model
        json tjson;
        tjson["prompt"] = config.prompt;
        if (!config.negative_prompt.empty()) {
            tjson["negative_prompt"] = config.negative_prompt;
        }
        tjson["model"] = kStaticModel;   // 低模静态
        tjson["texture"] = true;
        tjson["pbr"] = true;
        tjson["export_uv"] = true;
        tjson["auto_size"] = false;
        const std::string tt_url = base_url_ + kTextToModelPath;
        std::string tt_body, tt_err, tt_resp;
        bool tt_ok = false;
        CurlPerform(api_key_, "POST", tt_url, tjson.dump(),
                    /*auth*/ true, /*to_file*/ false, nullptr,
                    request_timeout_s_, &tt_resp, &tt_ok);
        if (!tt_ok) {
            result.error = "text-to-model 提交失败: " + tt_resp;
            return result;
        }
        const std::string stat_task = ParseTaskId(tt_resp, &tt_err);
        if (stat_task.empty()) {
            result.error = "text-to-model task 解析失败: " + tt_err;
            return result;
        }
        LOG(INFO) << "静态模型 task 已提交: " << stat_task;
        // (b) 轮询 text-to-model 完成。Auto Rig 的 input 用这个 text-to-model 的
        // task_id，不必下载静态版（我们只要确定它成功、task 可被 rig）。
        std::string tt_poll_err;
        bool tt_success = false;
        PollTask(api_key_, stat_task, base_url_, request_timeout_s_,
                 poll_deadline_s_, &tt_success, &tt_poll_err);
        if (!tt_success) {
            result.error = "text-to-model 任务未成功: " + tt_poll_err;
            return result;
        }
        // 静态模型 task 已成。用它做 Auto Rig 的 input（不需要真下载静态版）。
        rig_input = stat_task;
        describe_for_log = "静态 task_id=" + stat_task;
    } else {  // kDirectTask
        if (config.input_task_id.empty()) {
            result.error = "kDirectTask 模式需要 --input_task_id";
            return result;
        }
        rig_input = config.input_task_id;
        describe_for_log = "task_id=" + config.input_task_id;
    }

    // ---- 提交 Auto Rig ----
    const std::string rig_type = RigTypeToString(config.rig_type);
    const std::string spec = SpecToString(config.spec);
    const std::string model = ResolveRigModel(config);
    const std::string rig_body = BuildRigBody(rig_input, model, rig_type, spec);

    const std::string rig_url = base_url_ + kAutoRigPath;
    std::string rig_resp, rig_err;
    bool rig_ok = false;
    CurlPerform(api_key_, "POST", rig_url, rig_body,
                /*auth*/ true, /*to_file*/ false, nullptr,
                request_timeout_s_, &rig_resp, &rig_ok);
    if (!rig_ok) {
        result.error = "Auto Rig 提交失败: " + rig_resp;
        return result;
    }
    const std::string rig_task = ParseTaskId(rig_resp, &rig_err);
    if (rig_task.empty()) {
        result.error = "Auto Rig task 解析失败: " + rig_err;
        return result;
    }
    LOG(INFO) << "Auto Rig task 已提交: " << rig_task
              << " (input=" << describe_for_log << ", spec=" << spec
              << ", rig_type=" << rig_type << ", model=" << model << ")";

    // ---- 轮询 Auto Rig → model_url ----
    std::string rig_url_out, rig_poll_err;
    bool got_url = false;
    rig_url_out = PollTask(api_key_, rig_task, base_url_, request_timeout_s_,
                           poll_deadline_s_, &got_url, &rig_poll_err);
    if (!got_url) {
        result.error = "Auto Rig 任务未成功: " + rig_poll_err;
        return result;
    }

    // ---- 下载带骨骼 GLB ----
    const std::string glb_path = JoinGlbPath(output_dir, name);
    FILE* f = std::fopen(glb_path.c_str(), "wb");
    if (f == nullptr) {
        result.error = "无法写入文件: " + glb_path + " (errno="
            + std::to_string(errno) + ")";
        return result;
    }
    std::string dl_resp, dl_err;
    bool dl_ok = false;
    CurlPerform(api_key_, "GET", rig_url_out, /*body*/ "", /*auth*/ false,
                /*to_file*/ true, f, download_timeout_s_, &dl_resp, &dl_ok);
    std::fclose(f);
    if (!dl_ok) {
        result.error = "下载带骨骼 GLB 失败: " + dl_resp;
        std::remove(glb_path.c_str());
        return result;
    }

    result.glb_path = glb_path;
    LOG(INFO) << "带骨骼 GLB 已下载: " << glb_path
              << " (spec=" << spec << ", rig_type=" << rig_type << ")";
    return result;
}

}  // namespace gen3d
}  // namespace jpov

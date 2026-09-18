// tripo_client_resume_test — 守「断点续取」与「超时错误信息可救」两条不变量。
//
// 背景（2026-09-18 wallet 实测）：multiview 任务服务端已 success，但客户端
//   poll_deadline 到点后直接退出，**task_id 丢失**，产物没下载 → 只能重跑，
//   而重跑 = 白烧 credit。故本 PR 修两件事，本测试把它们钉死：
//
//   [A] 超时/失败的错误信息里**必须带 task_id**（否则无从续取）。
//   [B] ResumeByTaskId(task_id) **不重新提交**，直接轮询 + 下载产物。
//
// 手段：起一个本地 mock HTTP server 扮演 tripo，把 TripoClient 的 base_url
//   指过去。这样不烧 credit、不依赖外网（沙箱外网不稳时仍可跑），且可精确
//   构造「一直 running 直到超时」与「success 带产物」两种服务端行为。
//
// 负向验证（本测试已实际做过的破坏）：
//   1) 把超时 final_error 里的 task_id 去掉 → TestTimeoutErrorCarriesTaskId FAILED
//   2) ResumeByTaskId 内改成重新提交      → 调用数断言 FAILED

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include "glog/logging.h"
#include "tools/jpov/gen3d/static/tripo_client.h"

namespace {

using jpov::Gen3dInputMode;
using jpov::gen3d::Gen3dResult;
using jpov::gen3d::TripoClient;

int g_failures = 0;

void ExpectTrue(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  [ok] %s\n", what.c_str());
    } else {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

void ExpectContains(const std::string& hay, const std::string& needle,
                    const std::string& what) {
    ExpectTrue(hay.find(needle) != std::string::npos,
               what + "（子串 \"" + needle + "\"）");
}

// ---- 极简 mock HTTP server ----
// 只支持 GET（轮询任务状态 / 下载产物）。按「第 N 次请求」（从 0 起）决定回什么。
class MockServer {
public:
    explicit MockServer(std::function<std::string(int)> body_for_call)
        : body_fn_(std::move(body_for_call)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK_GE(listen_fd_, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        addr.sin_port = 0;  // 内核分配随机端口
        CHECK_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)), 0);
        CHECK_EQ(::listen(listen_fd_, 8), 0);
        socklen_t len = sizeof(addr);
        CHECK_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                               &len), 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { ServeLoop(); });
    }

    ~MockServer() {
        stop_.store(true);
        // 自连一次唤醒阻塞中的 accept。
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        a.sin_port = htons(port_);
        ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::close(fd);
        if (thread_.joinable()) thread_.join();
        ::close(listen_fd_);
    }

    // base_url 形如 http://127.0.0.1:<port>
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int call_count() const { return calls_.load(); }

private:
    void ServeLoop() {
        while (!stop_.load()) {
            int cfd = ::accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) break;
            if (stop_.load()) { ::close(cfd); break; }
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
            if (n > 0) buf[n] = '\0';
            const int nth = calls_.fetch_add(1);
            const std::string body = body_fn_(nth);
            const std::string resp = "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            ssize_t off = 0;
            while (off < static_cast<ssize_t>(resp.size())) {
                const ssize_t w =
                    ::send(cfd, resp.data() + off, resp.size() - off, 0);
                if (w <= 0) break;
                off += w;
            }
            ::close(cfd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::function<std::string(int)> body_fn_;
    std::atomic<bool> stop_{false};
    std::atomic<int> calls_{0};
    std::thread thread_;
};

// ---- [A] 超时错误信息必须带 task_id ----
// 服务端永远回 running → 客户端必超时。poll_deadline 调到 1s（poll 间隔 2s，
// 故第一次 sleep 后即判定窗口已过），测试秒回。
void TestTimeoutErrorCarriesTaskId() {
    std::printf("[A] 超时错误信息必须带 task_id\n");
    MockServer server([](int) {
        return std::string(R"({"code":0,"data":{"status":"running"}})");
    });

    TripoClient client("dummy-key", server.base_url());
    client.SetTimeouts(/*request*/ 5, /*poll_deadline*/ 1);

    const Gen3dResult r = client.ResumeByTaskId(
        Gen3dInputMode::kSingleImage, "task-abc-123",
        "/tmp/tripo_resume_test_out_a", "dummy");

    ExpectTrue(r.glb_path.empty(), "超时应失败（glb_path 为空）");
    ExpectContains(r.error, "task-abc-123",
                   "超时错误信息里必须带 task_id（否则无从续取）");
    ExpectContains(r.error, "--resume_task",
                   "超时错误信息应给出续取指引");
}

// ---- [B] ResumeByTaskId 对已完成任务能直接取货，且不重新提交 ----
// 第 0 次请求（查状态）→ success，产物 URL 指向本 mock 的 /blob；
// 第 1 次请求（下载）→ 假 GLB 字节。
void TestResumeDownloadsWithoutResubmit() {
    std::printf("[B] ResumeByTaskId 不重新提交，直接取货\n");

    const std::string fake_glb = "GLBTESTDATA-0123456789";
    std::string blob_url;  // 由下面 server 构造后再回填
    MockServer server([&blob_url, &fake_glb](int nth) {
        if (nth == 0) {
            // 注意：ParseModelUrl 读的是 data.output.model_url（不是 pbr_model；
            // pbr_model 只出现在任务查询响应的 output 里，本 mock 要走的是
            // 客户端实际解析的那个键，否则下载永远不触发）。
            return std::string(R"({"code":0,"data":{"status":"success",)")
                + R"("output":{"model_url":")" + blob_url + R"("}}})";
        }
        return fake_glb;
    });
    blob_url = server.base_url() + "/blob";

    const std::string out_dir = "/tmp/tripo_resume_test_out_b";
    ::system(("rm -rf " + out_dir).c_str());

    TripoClient client("dummy-key", server.base_url());
    client.SetTimeouts(/*request*/ 10, /*poll_deadline*/ 30);

    const Gen3dResult r = client.ResumeByTaskId(
        Gen3dInputMode::kSingleImage, "task-done-999", out_dir, "resumed");

    ExpectTrue(!r.glb_path.empty(), "已完成任务应能直接取到 GLB（不报超时）");

    // 关键：**只发生 2 次 HTTP 调用**（1 次查状态 + 1 次下载），
    // 没有任何“重新提交”的 POST —— 这就是“不重烧 credit”的证据。
    ExpectTrue(server.call_count() == 2,
               "应恰好 2 次 HTTP（查状态 + 下载），无重新提交；实际 "
               + std::to_string(server.call_count()));

    if (!r.glb_path.empty()) {
        std::string got;
        if (FILE* f = std::fopen(r.glb_path.c_str(), "rb")) {
            char buf[256];
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            got.assign(buf, n);
            std::fclose(f);
        }
        ExpectTrue(got == fake_glb, "下载内容应与 mock 返回字节一致");
    }
    ::system(("rm -rf " + out_dir).c_str());
}

// ---- [C] failed 状态也必须带 task_id（可追溯）----
void TestFailedCarriesTaskId() {
    std::printf("[C] failed 状态错误信息带 task_id\n");
    MockServer server([](int) {
        return std::string(R"({"code":0,"data":{"status":"failed"}})");
    });
    TripoClient client("dummy-key", server.base_url());
    client.SetTimeouts(/*request*/ 5, /*poll_deadline*/ 30);

    const Gen3dResult r = client.ResumeByTaskId(
        Gen3dInputMode::kSingleImage, "task-failed-777", "/tmp/x", "d");
    ExpectTrue(r.glb_path.empty(), "failed 任务不应产出 GLB");
    ExpectContains(r.error, "task-failed-777",
                   "failed 错误信息带 task_id 便于追溯");
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    std::printf("=== tripo_client_resume_test ===\n");
    TestTimeoutErrorCarriesTaskId();
    TestResumeDownloadsWithoutResubmit();
    TestFailedCarriesTaskId();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURES\n", g_failures);
    return 1;
}

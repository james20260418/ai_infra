// JPOV Strip2D Gold Image Unit Test
//
// 用 gold image 方法验证 Strip2DCommand 2D 条带渲染正确：
//   1. 画一个覆盖已知区域的 Z 字形 2D 三角形条带（像素坐标）
//   2. 保存为 PNG 到 output/jpov_strip2d_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

// ============ 测试应用 ============

class Strip2dGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- 构造 2D 三角形条带（与 generator 完全一致） ----
        std::vector<jpov::Vec2f> verts = {
            { 80.0f,  40.0f},
            {160.0f, 200.0f},
            {240.0f,  40.0f},
            {320.0f, 200.0f},
            {400.0f,  40.0f},
            {480.0f, 200.0f},
        };

        cmds->DrawStrip2D(verts, {1.0f, 0.2f, 0.2f, 1.0f});  // 红色
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/strip2d_diagonal_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/strip2d_diagonal_640x360.png";
}

// ============ 测试入口 ============

int main() {
    // 1. 加载 expected PNG
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!jpov::ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path;
    }
    LOG(INFO) << "Expected PNG loaded: " << expected_bytes.size() << " bytes";

    // 2. 渲染并保存为 PNG
    std::string outdir = jpov::GetOutputDir() + "jpov_strip2d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Strip2D Gold Test";
    cfg.headless = true;
    Strip2dGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. 加载渲染输出的 PNG
    std::vector<uint8_t> render_bytes;
    if (!jpov::ReadFileBytes(outpath, &render_bytes)) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath;
    }
    LOG(INFO) << "Rendered PNG: " << render_bytes.size() << " bytes";

    // 4. 二进制比较
    if (render_bytes.size() != expected_bytes.size()) {
        LOG(ERROR) << "Size mismatch: rendered=" << render_bytes.size()
                    << ", expected=" << expected_bytes.size();
        return 1;
    }

    bool pass = true;
    for (size_t i = 0; i < render_bytes.size(); ++i) {
        if (render_bytes[i] != expected_bytes[i]) {
            LOG(ERROR) << "Byte mismatch at offset " << i
                        << ": got 0x" << std::hex
                        << static_cast<int>(render_bytes[i])
                        << ", expected 0x"
                        << static_cast<int>(expected_bytes[i]);
            pass = false;
            break;
        }
    }

    if (pass) {
        LOG(INFO) << "TEST PASSED: strip2d gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: strip2d gold image mismatch";
        return 1;
    }
}

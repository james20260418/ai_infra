// JPOV RoundRect Gold Image Unit Test
//
// 用 gold image 方法验证 RoundRect2DCommand 圆角矩形渲染正确：
//   1. 画多种圆角半径的圆角矩形（包括 radius=0 退化情况）
//   2. 保存为 PNG 到 output/jpov_roundrect_gold_test/rendered.png
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

class RoundRectGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- roundrect #1: radius=0 → 退化为普通矩形（绿色） ----
        cmds->DrawRoundRect({40.0f, 30.0f}, {120.0f, 90.0f}, 0.0f,
                            {0.0f, 0.8f, 0.0f, 1.0f});

        // ---- roundrect #2: 小圆角半径 12px（蓝色） ----
        cmds->DrawRoundRect({200.0f, 30.0f}, {160.0f, 90.0f}, 12.0f,
                            {0.0f, 0.3f, 0.9f, 1.0f});

        // ---- roundrect #3: 大圆角半径 40px，正方形（红色） ----
        cmds->DrawRoundRect({60.0f, 150.0f}, {160.0f, 160.0f}, 40.0f,
                            {0.9f, 0.2f, 0.2f, 1.0f});

        // ---- roundrect #4: 中等圆角，边框非正方形（黄色半透明） ----
        cmds->DrawRoundRect({300.0f, 150.0f}, {200.0f, 120.0f}, 20.0f,
                            {0.9f, 0.8f, 0.1f, 0.8f});
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/roundrect_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/roundrect_640x360.png";
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
    std::string outdir = jpov::GetOutputDir() + "jpov_roundrect_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "RoundRect Gold Test";
    cfg.headless = true;
    RoundRectGoldTestApp app(cfg);
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
        LOG(INFO) << "TEST PASSED: roundrect gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: roundrect gold image mismatch";
        return 1;
    }
}

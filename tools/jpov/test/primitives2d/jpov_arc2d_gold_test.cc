// JPOV Arc2D Gold Image Unit Test
//
// 用 gold image 方法验证 Arc2DCommand 圆弧/扇形渲染正确：
//   1. 画多种圆弧/扇形变体（半圆、扇形、完整圆、负角度）
//   2. 保存为 PNG 到 output/jpov_arc2d_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/primitives2d/test_utils.h"

// ============ 测试应用 ============

class Arc2dGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- arc #1: 半圆 (180度，绿色) ----
        cmds->DrawArc2D({120.0f, 120.0f}, 80.0f, 0.0f, 180.0f,
                        {0.0f, 0.8f, 0.0f, 1.0f});

        // ---- arc #2: 扇形 (90度，蓝色) ----
        cmds->DrawArc2D({320.0f, 120.0f}, 80.0f, 45.0f, 90.0f,
                        {0.0f, 0.3f, 0.9f, 1.0f});

        // ---- arc #3: 完整圆 (360度，红色) ----
        cmds->DrawArc2D({520.0f, 120.0f}, 70.0f, 0.0f, 360.0f,
                        {0.9f, 0.2f, 0.2f, 1.0f});

        // ---- arc #4: 负角度扇形（顺时针，黄色半透明） ----
        cmds->DrawArc2D({160.0f, 280.0f}, 90.0f, 0.0f, -120.0f,
                        {0.9f, 0.8f, 0.1f, 0.8f});

        // ---- arc #5: 小扇形 (60度，青色) ----
        cmds->DrawArc2D({380.0f, 280.0f}, 70.0f, 30.0f, 60.0f,
                        {0.0f, 0.8f, 0.8f, 1.0f});

        // ---- arc #6: 大扇形 (270度，紫色) ----
        cmds->DrawArc2D({530.0f, 280.0f}, 60.0f, 0.0f, 270.0f,
                        {0.7f, 0.2f, 0.8f, 1.0f});
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/primitives2d/arc2d_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/primitives2d/arc2d_640x360.png";
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
    std::string outdir = jpov::GetOutputDir() + "jpov_arc2d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Arc2D Gold Test";
    cfg.headless = true;
    Arc2dGoldTestApp app(cfg);
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
        LOG(INFO) << "TEST PASSED: arc2d gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: arc2d gold image mismatch";
        return 1;
    }
}

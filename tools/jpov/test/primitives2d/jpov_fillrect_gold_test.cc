// JPOV FillRect Gold Image Unit Test
//
// 用 gold image 方法验证 FillRect2DCommand 复合矩形渲染正确：
//   1. 画多种 FillRect 组合（无圆角/圆角、无边框/有边框）
//   2. 保存为 PNG 到 output/jpov_fillrect_gold_test/rendered.png
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

class FillRectGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- #1: 无圆角+纯填充（绿色） ----
        cmds->DrawFillRect({30.0f, 30.0f}, {130.0f, 80.0f},
                           {0.0f, 0.8f, 0.0f, 1.0f},
                           {0.0f, 0.0f, 0.0f, 0.0f},
                           0.0f, 0.0f);

        // ---- #2: 带圆角+纯填充（蓝色） ----
        cmds->DrawFillRect({200.0f, 30.0f}, {150.0f, 80.0f},
                           {0.0f, 0.3f, 0.9f, 1.0f},
                           {0.0f, 0.0f, 0.0f, 0.0f},
                           0.0f, 15.0f);

        // ---- #3: 无圆角+带边框（红填充+黄边框） ----
        cmds->DrawFillRect({30.0f, 140.0f}, {160.0f, 80.0f},
                           {0.9f, 0.2f, 0.2f, 1.0f},
                           {0.9f, 0.8f, 0.1f, 1.0f},
                           6.0f, 0.0f);

        // ---- #4: 带边框+圆角（青填充+白边框） ----
        cmds->DrawFillRect({230.0f, 140.0f}, {160.0f, 90.0f},
                           {0.0f, 0.7f, 0.7f, 1.0f},
                           {1.0f, 1.0f, 1.0f, 1.0f},
                           4.0f, 12.0f);

        // ---- #5: 正方形+大圆角+细边框（紫填充+红边框） ----
        cmds->DrawFillRect({70.0f, 260.0f}, {130.0f, 130.0f},
                           {0.6f, 0.2f, 0.8f, 1.0f},
                           {0.9f, 0.1f, 0.1f, 1.0f},
                           3.0f, 30.0f);
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/primitives2d/fillrect_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/primitives2d/fillrect_640x360.png";
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
    std::string outdir = jpov::GetOutputDir() + "jpov_fillrect_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "FillRect Gold Test";
    cfg.headless = true;
    FillRectGoldTestApp app(cfg);
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
        LOG(INFO) << "TEST PASSED: fillrect gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: fillrect gold image mismatch";
        return 1;
    }
}

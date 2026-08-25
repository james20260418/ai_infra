// JPOV Text Gold Image Unit Test
//
// 用 gold image 方法验证 text 渲染正确：
//   1. 渲染字符串 "Hello JPOV!"（字号 48，居中）
//   2. 保存为 PNG 到 output/jpov_text_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// 读取文件全部字节
static bool ReadFileBytes(const std::string& path,
                          std::vector<uint8_t>* out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG(ERROR) << "Failed to open file: " << path;
        return false;
    }
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(out->data()), size)) {
        LOG(ERROR) << "Failed to read file: " << path;
        return false;
    }
    return true;
}

// ============ 测试应用 ============

class TextGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // 渲染分辨率 640x360 — 与 gold image 生成时一致
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- 文字：白色 "Hello JPOV!"，字号 48，居中 ----
        const char* text = "Hello JPOV!";
        cmds->DrawText(text, {kResW * 0.5f, kResH * 0.5f}, 48.0f,
                       jpov::kColorWhite,
                       jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinLatin);
    }
};

// 解析 expected PNG 路径：优先 $TEST_SRCDIR（bazel test 沙箱），
// 否则用 GetProjectRoot()（bazel run / 本地运行）。
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/font2d/hello_jpov_48_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/font2d/hello_jpov_48_640x360.png";
}

// ============ 测试入口 ============

int main() {
    // 1. 加载 expected PNG（git 管理，与单测同目录）
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path;
    }
    LOG(INFO) << "Expected PNG loaded: " << expected_bytes.size() << " bytes";

    // 2. 渲染并保存为 PNG 到 output/ 目录
    std::string outdir = jpov::GetOutputDir() + "jpov_text_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Text Gold Test";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    TextGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. 加载渲染输出的 PNG
    std::vector<uint8_t> render_bytes;
    if (!ReadFileBytes(outpath, &render_bytes)) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath;
    }
    LOG(INFO) << "Rendered PNG: " << render_bytes.size() << " bytes";

    // 4. 二进制字节比较
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
        LOG(INFO) << "TEST PASSED: text gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: text gold image mismatch";
        return 1;
    }
}

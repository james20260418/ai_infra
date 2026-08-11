// JPOV Chinese Text Gold Image Unit Test
//
// 验证中文文本渲染正确：
//   1. 渲染分辨率 1280x720，三行中文（字号 16/48/96）
//   2. 保存为 PNG 到 output/jpov_chinese_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// 字号常量（与 generator 一致）
static constexpr float kFontSizeSmall = 16.0f;
static constexpr float kFontSizeMedium = 32.0f;
static constexpr float kFontSizeLarge = 48.0f;

// 渲染分辨率
static constexpr float kResW = 1280.0f;
static constexpr float kResH = 720.0f;

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

class ChineseGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        float center_x = kResW * 0.5f;

        const char* line1 = reinterpret_cast<const char*>(u8"16px: 你好 JPOV! (16px atlas)");
        cmds->DrawText(line1, {center_x, 120.0f}, kFontSizeSmall,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinCJK);

        const char* line2 = reinterpret_cast<const char*>(u8"32px: 你好 JPOV! (32px atlas)");
        cmds->DrawText(line2, {center_x, 360.0f}, kFontSizeMedium,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinCJK);

        const char* line3 = reinterpret_cast<const char*>(u8"48px: 你好 楷体! (48px atlas, Kai)");
        cmds->DrawText(line3, {center_x, 600.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "Kai");

        // 行 4: 刀隶体（阿里妈妈刀隶体）
        const char* line4 = reinterpret_cast<const char*>(u8"刀隶体: 你好 刀隶! (48px, DaoLi)");
        cmds->DrawText(line4, {center_x, 60.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "DaoLi");

        // 行 5: 麦圆体（荆南麦圆体）
        const char* line5 = reinterpret_cast<const char*>(u8"麦圆体: 你好 麦圆! (48px, MaiYuan)");
        cmds->DrawText(line5, {center_x, 680.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "MaiYuan");
    }
};

// 解析 expected PNG 路径：优先 $TEST_SRCDIR（bazel test 沙箱），
// 否则用 GetProjectRoot()（bazel run / 本地运行）。
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/font2d/hello_chinese_jpov_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/font2d/hello_chinese_jpov_1280x720.png";
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
    std::string outdir = jpov::GetOutputDir() + "jpov_chinese_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Chinese Text Gold Test";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/LxgwWenKai-Regular.ttf", 0, "Kai"},
        {"tools/jpov/fonts/AlimamaDaoLiTi.ttf",      0, "DaoLi"},
        {"tools/jpov/fonts/KNMaiyuan-Regular.ttf",   0, "MaiYuan"},
    };
    ChineseGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;
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
        LOG(INFO) << "TEST PASSED: Chinese text gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: Chinese text gold image mismatch";
        return 1;
    }
}

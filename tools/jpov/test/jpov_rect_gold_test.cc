// JPOV Rect Gold Image Unit Test
//
// 用 gold image 方法验证 rect 渲染坐标正确：
//   1. 渲染一个已知位置的蓝色矩形（居中，640x360 的一半）
//   2. 保存为 PNG 到 output/jpov_rect_gold_test/rendered.png
//   3. 从 data/rect_centered_blue_640x360.b64.txt 加载 gold image
//      并逐像素比较（rgb tolerance = 2/255）
//
// 测试通过条件：全部像素 RGB 偏差 ≤ 2。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// ============ 辅助函数 ============

// 从 base64 文本文件读取 gold image（编码后的 PNG）
//
// gold image 用 base64 编码的 PNG 存储，运行时解码为 raw RGBA。
// 文件位于工程 data/ 目录下，不纳入 git 也不作为 cc_library src。
static bool LoadGoldImageFromTxt(const char* path, int* out_w, int* out_h,
                                 std::vector<uint8_t>* out_rgba) {
    // 读取 base64 文本
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOG(ERROR) << "Failed to open gold image txt: " << path;
        return false;
    }
    std::string b64((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    ifs.close();

    // 去掉空白字符
    std::string clean;
    clean.reserve(b64.size());
    for (char c : b64) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            clean.push_back(c);
        }
    }

    // 简单 base64 解码
    static const int kRev[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };

    std::vector<uint8_t> raw;
    raw.reserve(clean.size() / 4 * 3);
    for (size_t i = 0; i < clean.size(); i += 4) {
        if (i + 3 >= clean.size()) break;
        int a = kRev[static_cast<unsigned char>(clean[i])];
        int b = kRev[static_cast<unsigned char>(clean[i + 1])];
        int c = kRev[static_cast<unsigned char>(clean[i + 2])];
        int d = kRev[static_cast<unsigned char>(clean[i + 3])];
        if (a < 0 || b < 0) break;
        raw.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (c >= 0) {
            raw.push_back(static_cast<uint8_t>(((b & 0x0f) << 4) | (c >> 2)));
        }
        if (d >= 0) {
            raw.push_back(static_cast<uint8_t>(((c & 0x03) << 6) | d));
        }
    }

    // 用 stb_image 解码
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        raw.data(), static_cast<int>(raw.size()),
        &w, &h, &channels, 4);
    if (!pixels) {
        LOG(ERROR) << "Failed to decode gold image: " << stbi_failure_reason();
        return false;
    }
    *out_w = w;
    *out_h = h;
    out_rgba->assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

// 用 stb_image 从文件加载 PNG 并解码
static bool LoadPngFile(const char* path, int* out_w, int* out_h,
                        std::vector<uint8_t>* out_rgba) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        LOG(ERROR) << "Failed to load PNG " << path << ": "
                    << stbi_failure_reason();
        return false;
    }
    *out_w = w;
    *out_h = h;
    out_rgba->assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

// 比较两个 RGBA 像素缓冲
static bool ComparePixels(const std::vector<uint8_t>& actual,
                          const std::vector<uint8_t>& expected,
                          int w, int h, int max_rgb_diff) {
    CHECK_EQ(actual.size(), expected.size());
    CHECK_EQ(actual.size(), static_cast<size_t>(w) * h * 4);

    int max_diff = 0;
    int diff_count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * w * 4 + x * 4;
            int dr = std::abs(static_cast<int>(actual[idx]) -
                              static_cast<int>(expected[idx]));
            int dg = std::abs(static_cast<int>(actual[idx + 1]) -
                              static_cast<int>(expected[idx + 1]));
            int db = std::abs(static_cast<int>(actual[idx + 2]) -
                              static_cast<int>(expected[idx + 2]));
            int d = std::max({dr, dg, db});
            if (d > max_diff) max_diff = d;
            if (d > max_rgb_diff) ++diff_count;
        }
    }

    LOG(INFO) << "Pixel compare: max RGB diff=" << max_diff
              << ", pixels exceeding tolerance=" << diff_count
              << " out of " << w * h;
    return diff_count == 0;
}

// ============ 测试应用 ============

class RectGoldTestApp : public JPOV {
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
        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // ---- 矩形：蓝色，居中，大小为分辨率的 1/2 ----
        float rect_w = kResW * 0.5f;
        float rect_h = kResH * 0.5f;
        float rect_x = (kResW - rect_w) * 0.5f;
        float rect_y = (kResH - rect_h) * 0.5f;
        cmds->DrawRect({rect_x, rect_y}, {rect_w, rect_h}, jpov::kColorBlue);
    }
};

// ============ 测试入口 ============

int main() {
    // 1. 加载 gold image（从 base64 txt）
    // gold image base64 文件与单测源码放在一起，用 git 管理
    std::string gold_txt_path =
        jpov::GetProjectRoot() + "tools/jpov/test/rect_centered_blue_640x360.b64.txt";
    int gold_w = 0, gold_h = 0;
    std::vector<uint8_t> gold_rgba;
    if (!LoadGoldImageFromTxt(gold_txt_path.c_str(), &gold_w, &gold_h, &gold_rgba)) {
        LOG(FATAL) << "Gold image loading failed";
    }
    LOG(INFO) << "Gold image loaded: " << gold_w << "x" << gold_h;

    // 2. 渲染并保存为 PNG 到 output/ 目录
    std::string outdir = jpov::GetOutputDir() + "jpov_rect_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Rect Gold Test";
    cfg.headless = true;
    RectGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(gold_w);
    winfo.height = static_cast<float>(gold_h);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. 加载渲染输出的 PNG
    int render_w = 0, render_h = 0;
    std::vector<uint8_t> render_rgba;
    if (!LoadPngFile(outpath.c_str(), &render_w, &render_h, &render_rgba)) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath;
    }
    LOG(INFO) << "Rendered PNG loaded: " << render_w << "x" << render_h;

    // 4. 确保尺寸一致
    if (render_w != gold_w || render_h != gold_h) {
        LOG(ERROR) << "Dimension mismatch: rendered=" << render_w << "x" << render_h
                    << ", gold=" << gold_w << "x" << gold_h;
        return 1;
    }

    // 5. 逐像素比较
    bool pass = ComparePixels(render_rgba, gold_rgba,
                              gold_w, gold_h, 2);  // tolerance 2/255

    if (pass) {
        LOG(INFO) << "TEST PASSED: rect gold image match";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: rect gold image mismatch";
        return 1;
    }
}

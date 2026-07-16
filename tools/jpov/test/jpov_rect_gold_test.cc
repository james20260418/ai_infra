// JPOV Rect Gold Image Unit Test
//
// 用 gold image 方法验证 rect 渲染坐标正确：
//   1. 渲染一个已知位置的蓝色矩形（居中，640x360 的一半）
//   2. 保存为 PNG
//   3. 加载 gold image 并逐像素比较（rgb tolerance = 2/255）
//
// gold image 由 jpov_gold_generator 生成，通过 xxd 转为 C 头文件。
// 测试通过条件：全部像素 RGB 偏差 ≤ 2。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <glog/logging.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/rect_centered_blue_640x360.h"
#include "tools/common/utils.h"

// ============ 辅助函数 ============

// 用 stb_image 解码内嵌的 gold image PNG 数据
static bool LoadGoldImage(int* out_w, int* out_h,
                          std::vector<uint8_t>* out_rgba) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        tools_jpov_test_rect_centered_blue_640x360_png,
        tools_jpov_test_rect_centered_blue_640x360_png_len,
        &w, &h, &channels, 4);  // 强制 RGBA
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
    // 1. 加载 gold image
    int gold_w = 0, gold_h = 0;
    std::vector<uint8_t> gold_rgba;
    if (!LoadGoldImage(&gold_w, &gold_h, &gold_rgba)) {
        LOG(FATAL) << "Gold image loading failed";
    }
    LOG(INFO) << "Gold image loaded: " << gold_w << "x" << gold_h;

    // 2. 渲染并保存为 PNG
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

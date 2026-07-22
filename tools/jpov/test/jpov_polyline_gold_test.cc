// JPOV Polyline Gold Image Unit Test
//
// 用 gold image 方法验证 polyline 渲染坐标正确：
//   1. 渲染一条已知位置的红色折线（锯齿形，10px 线宽）
//   2. 保存为 PNG 到 output/jpov_polyline_gold_test/rendered.png
//   3. 解码 expected PNG 和 rendered PNG，逐像素 RGBA ±5 容差比较
//
// 测试通过条件：所有像素每个 RGBA 通道偏差 ≤ 5。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

// 在此文件中实现 stb_image（非 stb_image_write），因此不需要 STB_IMAGE_IMPLEMENTATION
// 在 renderer.cc 和 header 中已经通过其他方式处理了 stb 相关宏。
// 这里我们直接包含 stb_image.h 并在此编译单元定义实现。
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// ============ 测试应用 ============

class PolylineGoldTestApp : public JPOV {
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

        // ---- 折线：红色，10px 线宽，锯齿形 ----
        float line_width = 10.0f;
        std::vector<jpov::Vec2f> pts = {
            { 60.0f, 180.0f },
            { 160.0f, 60.0f },
            { 320.0f, 180.0f },
            { 480.0f, 300.0f },
            { 580.0f, 180.0f },
        };
        cmds->DrawPolyline(pts, jpov::kColorRed, line_width);
    }
};

// 解析 expected PNG 路径：优先 $TEST_SRCDIR（bazel test 沙箱），
// 否则用 GetProjectRoot()（bazel run / 本地运行）。
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/polyline_zigzag_red_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/polyline_zigzag_red_640x360.png";
}

// ============ 测试入口 ============

int main() {
    // 1. 获取 expected PNG 路径
    std::string expected_path = GetExpectedPngPath();

    // 2. 渲染并保存为 PNG 到 output/ 目录
    std::string outdir = jpov::GetOutputDir() + "jpov_polyline_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Polyline Gold Test";
    cfg.headless = true;
    PolylineGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. 用 stb_image 解码 expected PNG
    int exp_w = 0, exp_h = 0, exp_comp = 0;
    unsigned char* exp_pixels = stbi_load(expected_path.c_str(),
                                          &exp_w, &exp_h, &exp_comp, 4);
    if (!exp_pixels) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path
                    << " (" << stbi_failure_reason() << ")";
    }
    LOG(INFO) << "Expected: " << exp_w << "x" << exp_h
              << " RGBA (" << exp_comp << " native channels)";

    // 4. 用 stb_image 解码 rendered PNG
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                          &rnd_w, &rnd_h, &rnd_comp, 4);
    if (!rnd_pixels) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath
                    << " (" << stbi_failure_reason() << ")";
    }
    LOG(INFO) << "Rendered: " << rnd_w << "x" << rnd_h
              << " RGBA (" << rnd_comp << " native channels)";

    // 5. 尺寸检查
    if (exp_w != rnd_w || exp_h != rnd_h) {
        LOG(ERROR) << "Dimension mismatch: expected=" << exp_w << "x" << exp_h
                    << ", rendered=" << rnd_w << "x" << rnd_h;
        stbi_image_free(exp_pixels);
        stbi_image_free(rnd_pixels);
        return 1;
    }

    // 6. 逐像素 RGBA ±5 容差比较
    const int kTolerance = 5;
    const int total_pixels = exp_w * exp_h;
    int bad_pixels = 0;
    int max_channel_diff = 0;

    for (int i = 0; i < total_pixels; ++i) {
        for (int c = 0; c < 4; ++c) {
            int diff = std::abs(static_cast<int>(rnd_pixels[i * 4 + c]) -
                                static_cast<int>(exp_pixels[i * 4 + c]));
            if (diff > max_channel_diff) {
                max_channel_diff = diff;
            }
            if (diff > kTolerance) {
                ++bad_pixels;
                if (bad_pixels <= 5) {
                    int px = i % exp_w;
                    int py = i / exp_w;
                    LOG(ERROR) << "Pixel (" << px << "," << py
                               << ") channel[" << c
                               << "]: got " << static_cast<int>(rnd_pixels[i * 4 + c])
                               << ", expected " << static_cast<int>(exp_pixels[i * 4 + c])
                               << ", diff=" << diff
                               << " (tolerance=" << kTolerance << ")";
                }
            }
        }
    }

    stbi_image_free(exp_pixels);
    stbi_image_free(rnd_pixels);

    if (bad_pixels == 0) {
        LOG(INFO) << "TEST PASSED: polyline gold image match within "
                  << kTolerance << " RGBA tolerance ("
                  << total_pixels << " pixels, max channel diff="
                  << max_channel_diff << ")";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: " << bad_pixels << " pixels exceeded "
                    << kTolerance << " RGBA tolerance (max diff="
                    << max_channel_diff << ", total pixels="
                    << total_pixels << ")";
        return 1;
    }
}

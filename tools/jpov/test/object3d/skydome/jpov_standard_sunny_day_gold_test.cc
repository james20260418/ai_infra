// JPOV 标准晴天（standard_sunny_day）gold test —— smoke check
//
// 渲染"正午晴天"场景（天光 + 太阳 + 阴影），验证渲染链路跑通 + 输出非平凡
// 效果图。PBR 光照在 llvmpipe 下三稳态非确定，故不做逐像素颜色比对；效果
// 正确性由 Danis 肉眼查看 generator 产出的 gold image（standard_sunny_day_
// 1280x720.png）判断。本 test 校验 gold image 存在 + smoke check。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_standard_sunny_day_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_standard_sunny_day_test/";
}

// 仓库内 gold image 路径（generator 写入）。
std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/skydome/"
             "standard_sunny_day_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/skydome/standard_sunny_day_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 gold image 存在（generator 生成，供肉眼参考；缺失=回归）。
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_standard_sunny_day_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Standard Sunny Day Test";
    cfg.headless = true;
    jpov_standard_sunny_day::StandardSunnyDayApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // smoke check: 效果图非空
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath
        << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";
    LOG(INFO) << "Rendered: " << w << "x" << h;

    int nz = 0;
    for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
    stbi_image_free(px);
    float cov = static_cast<float>(nz) / (w * h);
    LOG(INFO) << "coverage=" << (cov * 100.0f) << "%";
    CHECK_GT(nz, 0) << "空场景";

    LOG(INFO) << "TEST PASSED: 标准晴天场景渲染链路跑通 (gold 见 "
        "standard_sunny_day_1280x720.png)";
    return 0;
}

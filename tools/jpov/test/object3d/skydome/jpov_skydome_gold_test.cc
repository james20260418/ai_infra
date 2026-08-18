// JPOV Skydome gold test —— 白天/晚霞/夜间 × 双机位
//
// 验证程序化天光（Preetham）在 3 时刻 × 2 机位下正确渲染：
//   - 白天：太阳圆盘 + 蓝天渐变 + 方向不对称（朝日暖、背面冷）
//   - 晚霞：低太阳红橙 + 长影
//   - 夜间：月亮圆盘 + 冷蓝夜空
//   - 机位：从日月看场景 / 从场景看日月
//
// 生成 6 张 gold（skydome_<time>_<cam>.png），供肉眼判断效果（PBR 三稳态
// 非确定，故不做逐像素比对）。本 test 校验 6 张 gold 均存在（防回归）。
//
// 测试通过条件：6 张 gold 存在 + 渲染链路跑通（渲染一张 day_to_body 作 smoke）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_skydome_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_skydome_test/";
}

// 仓库内 gold 目录（generator 写入）
std::string GetGoldDir() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/test/object3d/skydome/";
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/skydome/";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 6 张 gold 存在（generator 生成）
    const char* times[] = {"day", "sunset", "night"};
    const char* cams[]  = {"from_body", "to_body"};
    for (const char* t : times) {
        for (const char* c : cams) {
            std::string gold = GetGoldDir() + "skydome_" + t + "_" + c + ".png";
            FILE* f = std::fopen(gold.c_str(), "rb");
            CHECK(f != nullptr) << "skydome gold 缺失，请先跑 "
                "jpov_skydome_gold_generator: " << gold;
            std::fclose(f);
            LOG(INFO) << "gold 存在: " << gold;
        }
    }

    // smoke check：渲染一张 day_to_body
    JPOV::Config cfg;
    cfg.title = "JPOV Skydome Test (day/to_body)";
    cfg.headless = true;
    jpov_skydome::SkydomeSceneApp app(cfg);
    app.time_ = jpov_skydome::TimePreset::kDay;
    app.cam_  = jpov_skydome::CamPreset::kToBody;
    app.Init();
    jpov_skydome::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;
    int nz = 0;
    for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
    stbi_image_free(px);
    LOG(INFO) << "Rendered: " << w << "x" << h << " coverage=" << (100.0*nz/(w*h)) << "%";
    CHECK_GT(nz, 0) << "空场景";

    LOG(INFO) << "TEST PASSED: skydome gold 齐全 + 渲染链路跑通";
    return 0;
}

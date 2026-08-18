// JPOV 场景 gold test —— 阳光版
//
// 渲染阳光场景（石头墙 + 5×5 地面 + 桌/凳/盆栽，太阳平行光 direction=(0,-1,-1)
// + CSM 阴影），验证:
//   1. 太阳平行光（DirectionalLight）+ 级联阴影（CSM, ShadowConfig::Default()）
//      在完整多物体布景上正确渲染（替代 3 点光源）
//   2. 场景几何/材质与点光源版（jpov_scene_gold_test）一致
//
// 测试通过条件：渲染链路跑通 + 输出非平凡效果图。PBR 光照在 llvmpipe 下
// 三稳态非确定（leader #16 决策），故不做逐像素颜色比对；效果正确性由
// leader/Danis 肉眼查看 generator 产出的 gold image
// （scene_in_sun_1280x720.png）判断。本 test 额外校验 gold image 存在。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_scene_in_sun_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_scene_in_sun_test/";
}

// 仓库内 gold image 路径（generator 写入，供肉眼/比对参考）。
std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/scene_in_sun_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/scene_in_sun_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 gold image 存在（generator 生成，供肉眼参考；缺失=回归）
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_scene_in_sun_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Scene Test (sun)";
    cfg.headless = true;
    jpov_scene_in_sun::SceneSunApp app(cfg);
    app.Init();
    jpov_scene_in_sun::BuildScene(&app);

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

    LOG(INFO) << "TEST PASSED: 场景渲染链路跑通 (阳光版, gold 见 "
        "scene_in_sun_1280x720.png)";
    return 0;
}

// JPOV 太阳影子（最简版）gold test
//
// 验证 DirectionalLight + 正交 shadow map 最小实现。场景：平板 + 立柱，
// 太阳 direction=(-1,-1,1)，立柱在平板上投影子。
//
// 测试通过条件：渲染链路跑通 + 输出非空。PBR 光照 llvmpipe 三稳态非确定
//（leader #16 决策），不做逐像素比对；影子正确性由 leader/Danis 肉眼查看
// generator 产出的 gold image 判断。本 test 额外校验 gold image 存在。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/jpov_sun_shadow_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_sun_shadow_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/sun_shadow_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/sun_shadow_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_sun_shadow_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Sun Shadow Test";
    cfg.headless = true;
    jpov_sun_shadow::SunShadowApp app(cfg);
    app.Init();
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(4.0f, 0.1f, 4.0f));
    app.pillar_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 1.0f, 0.5f));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;
    LOG(INFO) << "Rendered: " << w << "x" << h;

    int nz = 0;
    for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
    stbi_image_free(px);
    float cov = static_cast<float>(nz) / (w * h);
    LOG(INFO) << "coverage=" << (cov * 100.0f) << "%";
    CHECK_GT(nz, 0) << "空场景";

    LOG(INFO) << "TEST PASSED: 太阳影子渲染链路跑通 (gold 见 sun_shadow_1280x720.png)";
    return 0;
}

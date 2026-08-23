// JPOV box gold test —— 验证 MeshData::MakeBox + PBRMaterial 纯色构造
//
// 渲染 box 场景（地面 + 立柱 + 斜柱），验证：
//   1. MakeBox 构造的空间盒几何正确（6 面、绕序、法线）
//   2. PBRMaterial::SolidColor / SolidColorMR 纯色材质构造可用
//
// 测试通过条件：渲染链路跑通 + 输出非空效果图。注意：PBR 光照在 llvmpipe
// 下三稳态非确定（leader #16 决策），故不做逐像素颜色比对；box 几何正确性
// 由 leader/Danis 肉眼查看 generator 产出的 gold image 判断。本 test 额外
// 校验 gold image 存在（防回归时误删/忘生成）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/object3d/jpov_box_gold_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_box_test/";
}

// 仓库内 gold image 路径（generator 写入，供肉眼/比对参考）。
std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/box_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/object3d/box_1280x720.png";
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
            "jpov_box_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Box Test";
    cfg.headless = true;
    jpov_box::BoxApp app(cfg);
    app.Init();
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        2.0f, 0.1f, 3.0f));
    app.pillar_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        0.5f, 1.0f, 0.2f));
    app.slanted_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        0.7f, 0.7f, 0.35f));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // smoke check: 效果图非空。
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

    // ---- 光照平均颜色对比（Danis 决策）：8×8 平铺 ROI 块内平均色对比，
    //      输出最大的 RGB 通道差异，作为光照 gold 的硬性门禁。
    const double kLightThreshold = 25.0;
    const std::string gold_path = GetGoldPath();  // reuse existing GetGoldPath()
    if (FILE* f = std::fopen(gold_path.c_str(), "rb")) {
        std::fclose(f);
        const double max_diff = jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
        LOG(INFO) << "LIGHT COMPARE: gold vs rendered "
                  << "max-channel-mean-diff (tile 8x8) = " << max_diff
                  << " (threshold=" << kLightThreshold << ")";
        if (max_diff < 0) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold 或 rendered 读取/尺寸错误）";
            return 1;
        }
        if (max_diff > kLightThreshold) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: max-channel-mean-diff="
                       << max_diff << " > threshold=" << kLightThreshold
                       << " → 光照回归！";
            return 1;
        }
        LOG(INFO) << "LIGHT COMPARE PASSED: max-channel-mean-diff="
                  << max_diff << " <= threshold=" << kLightThreshold;
    }

    LOG(INFO) << "TEST PASSED: box 场景渲染链路跑通 (gold 见 box_1280x720.png)";
    return 0;
}

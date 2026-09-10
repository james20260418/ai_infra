// JPOV skeleton gold test（M1）— 蒙皮 T-rest 蓝人 + object3d 对照（照猫画虎 sunny_day）
//
// 与 generator 共用 jpov_skeleton_gold_common.h 场景，渲一帧到 temp，再与仓库 gold
// （human_skeleton_gold_t_rest_1280x720.png）做 ROI 平均色对比（同 jpov_scene_in_sun_gold_test）。
// PBR/llvmpipe 三稳态非确定 → 不做逐像素，用 8×8 平铺 ROI 块内平均通道最大差作为硬门禁（阈值 25）。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/skeleton/jpov_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {
std::string OutDir() { return jpov::GetOutputDir() + "jpov_skeleton_gold_test/"; }
std::string GoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) { std::string s = e; if (!s.empty() && s.back()!='/') s.push_back('/');
             return s + "__main__/tools/jpov/test" + jpov_skeleton_gold::GetGoldRelPath(); }
    return jpov::GetProjectRoot() + "tools/jpov/test" + jpov_skeleton_gold::GetGoldRelPath();
}
std::string Assets() { return jpov::GetProjectRoot() + "tools/jpov/test/object3d/scene_assets/"; }
std::string Male()  { return jpov::GetProjectRoot() + "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb"; }
}  // namespace

int main() {
    // 校验仓库 gold 存在（generator 生成；缺失=回归）
    const std::string gold_path = GoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold 缺失，请先跑 jpov_skeleton_gold_generator: " << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold 存在: " << gold_path;
    }

    const std::string outdir = OutDir();
    std::string mk = "mkdir -p " + outdir;
    std::system(mk.c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Skeleton Gold Test (T-rest)";
    cfg.headless = true;
    jpov_skeleton_gold::SkeletonGoldApp app(cfg);
    app.Init();
    jpov_skeleton_gold::BuildScene(&app, Assets(), Male());

    jpov::WindowInfo winfo;
    winfo.width  = (float)jpov_skeleton_gold::kOutW;
    winfo.height = (float)jpov_skeleton_gold::kOutH;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // smoke：效果图非空
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "渲染 PNG 读取失败: " << outpath;
    int nz = 0;
    for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
    stbi_image_free(px);
    float cov = static_cast<float>(nz) / (w * h);
    LOG(INFO) << "coverage=" << (cov * 100.0f) << "%";
    CHECK_GT(nz, 0) << "空场景";

    // ROI 平均色对比（同 sunny_day；阈值 25，兼容 llvmpipe PBR 非确定）
    const double kThreshold = 25.0;
    const double max_diff = jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
    LOG(INFO) << "LIGHT COMPARE: gold vs rendered max-channel-mean-diff (8x8) = "
              << max_diff << " (threshold=" << kThreshold << ")";
    if (max_diff < 0) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold/rendered 读取或尺寸错误）";
        return 1;
    }
    if (max_diff > kThreshold) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: diff=" << max_diff << " > " << kThreshold;
        return 1;
    }
    LOG(INFO) << "jpov_skeleton_gold_test (T-rest) PASSED";
    return 0;
}

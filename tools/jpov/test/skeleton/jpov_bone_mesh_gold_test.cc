// JPOV 火柴人 gold test —— 渲染门禁
//
// 与 generator 共用 jpov_bone_mesh_gold_common.h 场景，渲一帧到 temp，再与仓库 gold
// （bone_mesh_stickman_1280x720.png）做 ROI 平均色对比（同 jpov_skeleton_gold_test）。
// PBR/llvmpipe 三稳态非确定 → 不做逐像素，用 8×8 平铺 ROI 块内平均通道最大差作为硬门禁。
//
// 额外门禁：在火柴人包围盒 ROI 内统计“强红”像素，确认火柴人真的渲染出来了 ——
// 若 mesh 生成失败/被剔除/渲染路径断，该计数会塌向 0。
// （不用全图红色占比：会被地面/天空淹没；也不用基线帧相减：那要多存一张图。）
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/skeleton/jpov_bone_mesh_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string OutDir() { return jpov::GetOutputDir() + "jpov_bone_mesh_gold_test/"; }

std::string GoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') s.push_back('/');
        return s + "__main__/tools/jpov/test" +
               jpov_bone_mesh_gold::GetGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_bone_mesh_gold::GetGoldRelPath();
}

}  // namespace

int main() {
    const std::string gold_path = GoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold 缺失，请先跑 jpov_bone_mesh_gold_generator: "
                            << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold 存在: " << gold_path;
    }

    const std::string outdir = OutDir();
    std::string mk = "mkdir -p " + outdir;
    std::system(mk.c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Bone Mesh Gold Test";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_bone_mesh_gold::BoneMeshGoldApp app(cfg);
    app.Init();
    jpov_bone_mesh_gold::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // ---- smoke：效果图非空 ----
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;

    int nz = 0;
    for (int i = 0; i < w * h; ++i) {
        if (px[i * 4 + 3] > 0) {
            ++nz;
        }
    }
    CHECK_GT(nz, 0) << "空场景";
    LOG(INFO) << "Rendered: " << w << "x" << h;

    // ---- 火柴人可见性门禁：火柴人包围盒内统计“强红”像素 ----
    //
    // 为何限定 ROI：整图数红像素会被地面/天空的颜色淹没（实测判据近乎恒真）；
    // 而屏幕角落的 x/y/z 轴标签也是红色，不排除就会污染计数。
    // 所以只在火柴人所在的矩形区域内统计，且用较严的“强红”判据
    //（火柴人是纯红纯色材质，光照后仍远高于其它物体）。
    //
    // 实测：火柴人 ROI 内强红 = 463 像素；不画火柴人时同 ROI = 0 像素。
    // 阈值取 100 —— 远高于噪声，又能在火柴人消失时可靠失败。
    {
        const int kRoiX0 = 294, kRoiY0 = 139, kRoiX1 = 410, kRoiY1 = 232;
        CHECK_LT(kRoiX1, w) << "火柴人 ROI 超出图像宽度，请同步更新 ROI 常量";
        CHECK_LT(kRoiY1, h) << "火柴人 ROI 超出图像高度，请同步更新 ROI 常量";

        int red_px = 0;
        for (int y = kRoiY0; y < kRoiY1; ++y) {
            for (int x = kRoiX0; x < kRoiX1; ++x) {
                const int i = (y * w + x) * 4;
                const int r = px[i + 0];
                const int g = px[i + 1];
                const int b = px[i + 2];
                if (r > 120 && g < 90 && b < 90) {
                    ++red_px;
                }
            }
        }
        stbi_image_free(px);
        px = nullptr;

        LOG(INFO) << "火柴人可见性: ROI 强红像素 = " << red_px;
        CHECK_GT(red_px, 100)
            << "火柴人 ROI 内强红像素过少（" << red_px
            << "），火柴人可能未渲染出来";
    }

    // ---- 光照平均色 ROI 对比（同 jpov_skeleton_gold_test 门禁）----
    const double kLightThreshold = 25.0;
    const double max_diff =
        jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
    LOG(INFO) << "LIGHT COMPARE: max-channel-mean-diff (tile 8x8) = " << max_diff
              << " (threshold=" << kLightThreshold << ")";
    if (max_diff < 0) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold/rendered 读取或尺寸错误）";
        return 1;
    }
    if (max_diff > kLightThreshold) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: " << max_diff
                   << " > threshold=" << kLightThreshold << " → 回归！";
        return 1;
    }

    LOG(INFO) << "TEST PASSED: 火柴人渲染链路跑通 (gold 见 "
              << jpov_bone_mesh_gold::GetGoldRelPath() << ")";
    return 0;
}

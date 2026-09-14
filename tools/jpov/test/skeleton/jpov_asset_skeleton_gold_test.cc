// JPOV 资产骨架 gold test —— 渲染门禁
//
// 与 generator 共用 jpov_asset_skeleton_gold_common.h 场景：渲一帧到 temp，与仓库 gold
// （skeleton_from_glb_fbx_1280x720.png）做 ROI 平均色对比（同 jpov_bone_mesh_gold_test）。
// PBR/llvmpipe 三稳态非确定 → 不做逐像素，用 8×8 平铺 ROI 块内平均通道最大差作为硬门禁。
//
// 额外门禁：在中央 ROI（两根骨人所在区域）内分别统计"强红"（glb 骨人）与"强蓝"
// （fbx 骨人）像素，确认**两根骨人都真的渲染出来了** —— 任一条链路断（loader 失败 /
// mesh 生成失败 / 渲染路径断），对应颜色计数会塌向 0。
// ROI 须避开屏幕角上的红字轴标签（"z/x/y" 也是红色，会污染计数）。
//
// 阈值校准（2026-09-14 生成 gold 时实测）：见下方常量处注释。
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/skeleton/jpov_asset_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string OutDir() { return jpov::GetOutputDir() + "jpov_asset_skeleton_gold_test/"; }

std::string GoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') s.push_back('/');
        return s + "__main__/tools/jpov/test" +
               jpov_asset_skeleton_gold::GetGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_asset_skeleton_gold::GetGoldRelPath();
}

// 中央 ROI：覆盖两根骨人（重叠）区域，避开三处红字轴标签。
constexpr int kRoiX0 = 280, kRoiY0 = 120, kRoiX1 = 430, kRoiY1 = 250;

// 可见性阈值：正测值见注释（generator 生成 gold【2026-09-14】实测原值，重复跑字节级一致），
// 取 ~40% 留余量；对应骨人未渲染时该计数 = 0，可可靠失败。
constexpr int kMinRedPx = 50;    // 实测 glb 骨人强红 = 119
constexpr int kMinBluePx = 150;  // 实测 fbx 骨人强蓝 = 392

}  // namespace

int main() {
    const std::string gold_path = GoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold 缺失，请先跑 jpov_asset_skeleton_gold_generator: "
                            << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold 存在: " << gold_path;
    }

    const std::string outdir = OutDir();
    std::string mk = "mkdir -p " + outdir;
    std::system(mk.c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Asset Skeleton Gold Test";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    jpov_asset_skeleton_gold::AssetSkeletonGoldApp app(cfg);
    app.Init();
    jpov_asset_skeleton_gold::BuildScene(&app);

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

    // ---- 双色可见性门禁：ROI 内分别统计"强红"（glb 骨人）/ "强蓝"（fbx 骨人）----
    //
    // 判据说明：火柴人是纯色材质，光照后仍显著偏离其它物体；"强红"用 r>120 且 g,b<90，
    // "强蓝"用 b>120 且 r,g<90 —— 两者互相排斥，天空（浅蓝、r 高）与地面（灰、三通道近等）
    // 均被排除；三处红字轴标签在 ROI 之外。
    {
        CHECK_LT(kRoiX1, w) << "ROI 超出图像宽度，请同步更新 ROI 常量";
        CHECK_LT(kRoiY1, h) << "ROI 超出图像高度，请同步更新 ROI 常量";

        int red_px = 0;
        int blue_px = 0;
        for (int y = kRoiY0; y < kRoiY1; ++y) {
            for (int x = kRoiX0; x < kRoiX1; ++x) {
                const int i = (y * w + x) * 4;
                const int r = px[i + 0];
                const int g = px[i + 1];
                const int b = px[i + 2];
                if (r > 120 && g < 90 && b < 90) {
                    ++red_px;
                }
                if (b > 120 && r < 90 && g < 90) {
                    ++blue_px;
                }
            }
        }
        stbi_image_free(px);
        px = nullptr;

        LOG(INFO) << "骨人可见性: ROI 强红(glb) = " << red_px
                  << " / 强蓝(fbx) = " << blue_px;
        CHECK_GT(red_px, kMinRedPx)
            << "ROI 内强红像素过少（" << red_px
            << "），glb 骨人可能未渲染出来";
        CHECK_GT(blue_px, kMinBluePx)
            << "ROI 内强蓝像素过少（" << blue_px
            << "），fbx 骨人可能未渲染出来";
    }

    // ---- 光照平均色 ROI 对比（同 jpov_bone_mesh_gold_test 门禁）----
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

    LOG(INFO) << "TEST PASSED: 资产骨架（glb + fbx）渲染链路跑通 (gold 见 "
              << jpov_asset_skeleton_gold::GetGoldRelPath() << ")";
    return 0;
}

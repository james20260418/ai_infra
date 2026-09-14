// JPOV FBX 观察器 gold test —— 渲染门禁
//
// 与 generator 共用 jpov_fbx_pose_gold_common.h（= 直接跑观察器本体），渲三帧到 temp：
//   ① kGoldTimeSeconds（第 60 帧）→ 与仓库 gold 做 ROI 平均色对比（同其它 gold 门禁）；
//   ② kOtherTimeSeconds（第 150 帧）→ 必须与 ① 不同（证明"时间→位姿→几何"确实在更新，
//      否则两帧会渲成同一张图）；
//   ③ rest 模式（identity 位姿）→ 必须与 ① 不同（证明 rest 复选框真的换了位姿来源）。
//
// 另外一道可见性门禁：场景里唯一的红色物体就是火柴人，故全图"强红"像素数若塌向 0，
// 说明火柴人没渲出来（mesh 生成失败/被剔除/链路断）。用整图计数而非 ROI：
// headless 出图不画面板与文字，没有任何其它红色来源（轴标签之类都在面板/交互路径里）。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/fbx_viewer/jpov_fbx_pose_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string OutDir() { return jpov::GetOutputDir() + "jpov_fbx_pose_gold_test/"; }

std::string GoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/tools/jpov/test" +
               jpov_fbx_pose_gold::GetGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_fbx_pose_gold::GetGoldRelPath();
}

// 整图"强红"像素数（火柴人是纯红材质，光照后仍远高于其它物体）。
long long CountStrongRedPixels(const std::string& png) {
    int w = 0;
    int h = 0;
    int c = 0;
    unsigned char* px = stbi_load(png.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "无法读取 PNG: " << png;
    long long n = 0;
    for (int i = 0; i < w * h; ++i) {
        const int r = px[i * 4 + 0];
        const int g = px[i * 4 + 1];
        const int b = px[i * 4 + 2];
        if (r > 120 && g < 90 && b < 90) {
            ++n;
        }
    }
    stbi_image_free(px);
    return n;
}

// 逐像素差异统计（两图尺寸须一致）：最大通道绝对差 + 差>阈值的像素数。
// 用途：小目标（火柴人只占全图 ~1% 像素）在 8×8 ROI 平均色里会被稀释到看不清
// （PR #74 踩过的坑）——"换时刻/换模式画面必须变"这类门禁要用逐像素判据。
void PerPixelDiffStats(const std::string& png_a, const std::string& png_b,
                       int* out_max_abs, long long* out_count_gt30) {
    int wa = 0;
    int ha = 0;
    int ca = 0;
    int wb = 0;
    int hb = 0;
    int cb = 0;
    unsigned char* a = stbi_load(png_a.c_str(), &wa, &ha, &ca, 4);
    unsigned char* b = stbi_load(png_b.c_str(), &wb, &hb, &cb, 4);
    CHECK(a != nullptr) << "无法读取 PNG: " << png_a;
    CHECK(b != nullptr) << "无法读取 PNG: " << png_b;
    CHECK(wa == wb && ha == hb) << "两图尺寸不一致: " << wa << "x" << ha << " vs "
                               << wb << "x" << hb;
    int max_abs = 0;
    long long count = 0;
    for (int i = 0; i < wa * ha; ++i) {
        int d = 0;
        for (int ch = 0; ch < 3; ++ch) {
            const int da = std::abs(static_cast<int>(a[i * 4 + ch]) -
                                    static_cast<int>(b[i * 4 + ch]));
            if (da > d) {
                d = da;
            }
        }
        if (d > max_abs) {
            max_abs = d;
        }
        if (d > 30) {
            ++count;
        }
    }
    stbi_image_free(a);
    stbi_image_free(b);
    *out_max_abs = max_abs;
    *out_count_gt30 = count;
}

}  // namespace

int main() {
    const std::string gold_path = GoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold 缺失，请先跑 jpov_fbx_pose_gold_generator: "
                            << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold 存在: " << gold_path;
    }

    const std::string outdir = OutDir();
    const std::string mk = "mkdir -p " + outdir;
    std::system(mk.c_str());
    const std::string out_gold_frame = outdir + "frame_gold_time.png";
    const std::string out_other_frame = outdir + "frame_other_time.png";
    const std::string out_rest_pose = outdir + "rest_pose.png";

    // ── 三帧都由观察器本体渲出（同一 Config / 同一 OneIteration）──
    {
        JPOV::Config cfg = jpov_fbx_pose_gold::MakeConfig("JPOV FBX Pose Gold Test");
        jpov_fbx_viewer::FbxViewerApp app(cfg);
        jpov_fbx_pose_gold::SetupApp(&app, jpov_fbx_pose_gold::kGoldTimeSeconds,
                                     /*rest_pose*/ false);
        jpov_fbx_pose_gold::RenderFrame(&app, out_gold_frame.c_str());
        app.anim_time_seconds_ = jpov_fbx_pose_gold::kOtherTimeSeconds;
        jpov_fbx_pose_gold::RenderFrame(&app, out_other_frame.c_str());
        app.rest_pose_mode_ = true;
        jpov_fbx_pose_gold::RenderFrame(&app, out_rest_pose.c_str());
        app.Finalize();
    }

    // ── 可见性门禁：火柴人必须渲出来了 ──
    // 实测（640×360 出图、初机位 R≈4.6）：强红 ≈145 px；火柴人缺失时为 0。
    // 门禁取 60 —— 远高于噪声、又能在火柴人消失时可靠失败。
    {
        const long long red = CountStrongRedPixels(out_gold_frame);
        LOG(INFO) << "火柴人可见性: 全图强红像素 = " << red;
        CHECK_GT(red, 60)
            << "强红像素过少（" << red << "），火柴人可能未渲染出来";
    }

    // ── 动态门禁：换时刻必须换画面（时间→位姿→几何这条链真的在跑）──
    // 用逐像素判据（见 PerPixelDiffStats 注释：8×8 均值会把小火柴人的变化稀释到 ~7）。
    {
        int max_abs = 0;
        long long moved_px = 0;
        PerPixelDiffStats(out_gold_frame, out_other_frame, &max_abs, &moved_px);
        LOG(INFO) << "动态门禁: 第 60 帧 vs 第 150 帧 逐像素 max-abs=" << max_abs
                  << " 差>30 像素数=" << moved_px;
        CHECK_GT(max_abs, 60) << "不同时刻两帧几乎没有像素差（位姿没更新？）";
        CHECK_GT(moved_px, 100) << "不同时刻两帧变化像素过少: " << moved_px;
    }

    // ── rest 模式门禁：identity 位姿必须与动画帧不同 ──
    {
        int max_abs = 0;
        long long moved_px = 0;
        PerPixelDiffStats(out_gold_frame, out_rest_pose, &max_abs, &moved_px);
        LOG(INFO) << "rest 门禁: 动画帧 vs rest(identity) 逐像素 max-abs=" << max_abs
                  << " 差>30 像素数=" << moved_px;
        CHECK_GT(max_abs, 60) << "勾选 rest 后画面没变（位姿来源没切？）";
        CHECK_GT(moved_px, 100) << "rest 与动画帧变化像素过少: " << moved_px;
    }

    // ── gold 平均色 ROI 对比（同其它 gold 门禁）──
    constexpr double kLightThreshold = 25.0;
    const double max_diff =
        jpov::CompareLightMeanRoiPng(gold_path, out_gold_frame, 8, 8);
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

    LOG(INFO) << "TEST PASSED: FBX 动作渲染链路跑通 (gold 见 "
              << jpov_fbx_pose_gold::GetGoldRelPath() << ")";
    return 0;
}

// JPOV 火柴人 gold test —— 渲染门禁
//
// 与 generator 共用 jpov_bone_mesh_gold_common.h 场景，渲一帧到 temp，再与仓库 gold
// （bone_mesh_stickman_1280x720.png）做 ROI 平均色对比（同 jpov_skeleton_gold_test）。
// PBR/llvmpipe 三稳态非确定 → 不做逐像素，用 8×8 平铺 ROI 块内平均通道最大差作为硬门禁。
//
// 额外门禁：把本帧与“同一场景但不画火柴人”的基线帧（_nobone.png）相减，
// 差异像素只可能来自火柴人 —— 若 mesh 生成失败/被剔除/渲染路径断，差异会塌向 0。
// （不用“红色像素占比”：场景砖地面本身偏红，会把判据淹没，实测无效。）
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

// “无火柴人”基线图路径（generator 产出）。
std::string NoBonePath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') s.push_back('/');
        return s + "__main__/tools/jpov/test" +
               jpov_bone_mesh_gold::GetNoBoneRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_bone_mesh_gold::GetNoBoneRelPath();
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

    // ---- 火柴人可见性门禁：与“无火柴人”基线图比对 ----
    //
    // 为何不用“红色像素占比”：场景的**砖地面本身偏红**，会把判据淹没 ——
    // 实测去掉火柴人后红色像素仅从 11.22% 降到 10.92%，占比阈值形同虚设。
    //
    // 正确做法：把本帧与“同一场景但不画火柴人”的基线帧相减，只对差异像素计数。
    // 差异像素只可能来自火柴人（场景其余部分完全相同），对“没画出来”高度敏感。
    // 基线图由 generator 同时产出（bone_mesh_stickman_1280x720_nobone.png）。
    {
        const std::string nobone = NoBonePath();
        int wb = 0, hb = 0, cb = 0;
        unsigned char* pb = stbi_load(nobone.c_str(), &wb, &hb, &cb, 4);
        CHECK(pb != nullptr)
            << "无火柴人基线图缺失，请先跑 jpov_bone_mesh_gold_generator: "
            << nobone;
        CHECK_EQ(wb, w) << "基线图与渲染图宽度不符";
        CHECK_EQ(hb, h) << "基线图与渲染图高度不符";

        // 逐像素最大通道差 > 25 计为“有差异”（避开 llvmpipe 微抖）。
        int diff_px = 0;
        int max_ch = 0;
        for (int i = 0; i < w * h; ++i) {
            const int dr = std::abs(static_cast<int>(px[i * 4 + 0]) - pb[i * 4 + 0]);
            const int dg = std::abs(static_cast<int>(px[i * 4 + 1]) - pb[i * 4 + 1]);
            const int db = std::abs(static_cast<int>(px[i * 4 + 2]) - pb[i * 4 + 2]);
            const int d = std::max(dr, std::max(dg, db));
            if (d > 25) {
                ++diff_px;
            }
            max_ch = std::max(max_ch, d);
        }
        stbi_image_free(pb);
        stbi_image_free(px);
        px = nullptr;

        LOG(INFO) << "火柴人可见性: diff_px=" << diff_px
                  << " (" << (100.0 * diff_px / (w * h)) << "%), max_ch=" << max_ch;
        // 实测：火柴人贡献 ~1977 差异像素（0.86%），max_ch≈172。
        // 阈值取 500 像素 —— 既远高于噪点，又能在火柴人消失（diff→0）时可靠失败。
        CHECK_GT(diff_px, 500)
            << "与无火柴人基线几乎无差异（diff_px=" << diff_px
            << "），火柴人可能未渲染出来";
        CHECK_GT(max_ch, 60)
            << "最大通道差异过小（" << max_ch << "），火柴人可能未渲染出来";
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

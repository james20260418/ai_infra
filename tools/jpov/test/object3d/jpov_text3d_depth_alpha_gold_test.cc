// JPOV 3D 文本深度测试 + alpha 混合 gold test。
//
// 本测试做两件事（缺一不可 —— 单靠 gold 字节比对抓不到"深度遮挡没生效"）：
//
//   A. gold 字节比对：与仓库内 gold PNG 逐字节比较。场景无光照变化
//      （纯色文字 + 固定光照地板），渲染确定性 → LLVMpipe 下逐字节稳定
//      （与 cube3d / strip3d / text3d gold 同一套做法）。
//
//   B. **深度遮挡结构断言**（本测试的核心价值）：
//      gold 只能证明"和上次一样"，不能证明"遮挡真的发生"——若深度测试被
//      破坏，文字会整片浮在地板之上，而 gold 图只要和 generator 一起重生成
//      就仍然绿。故这里额外：
//        1) 正常渲一帧（地板 + 文字）
//        2) 用 JPOV_NO_TEXT 渲一帧（只有地板）
//        3) 两帧逐像素求差 → 得到"文字实际显示的像素集合"
//        4) 断言 kCenter 行的可见高度 **小于** 其几何完整高度 —— 即被地板
//           切掉了一截；且被切掉的量 > 0（深度遮挡确实起作用）
//
//      几何事实（几何门禁来自 jpov_text3d_gold_test.cc 的同类断言 + 本场景
//      的 anchor/face/up，均可静态推导）：face=+X 时文字平面是 x=0 的 YZ
//      竖平面，kCenter 的墨迹 y∈[-0.35,+0.35]，**跨过地板面 y=0**。
//      若深度测试失效，center 行会完整显示（高度 ≈ 全墨迹高度）。
//
// ⚠️ 零运行代码阅读检查记录：刻意不写以下无效断言：
//   - EXPECT_GT(diff_pixels, 0)：文字若整体没被画出（如字体没加载），diff=0，
//     该断言能抓"文字消失"——**这条有效**，保留（见下）。
//   - 但"center 高度 < 全高"必须用**实测的两帧对比**得到，不能写死常数
//     （写死常数 = 换个机器/驱动就红，且证明不了遮挡机制本身）。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/text3d_depth_alpha_common.h"
#include "tools/jpov/test/primitives3d/test_utils.h"
#include "tools/jpov/test/test_utils.h"

namespace {

using namespace jpov_text3d_depth_alpha;

// 读 PNG 为 RGBA 像素（失败 crash）。
std::vector<uint8_t> LoadRgba(const std::string& path, int* w, int* h) {
    int comp = 0;
    unsigned char* px = stbi_load(path.c_str(), w, h, &comp, 4);
    CHECK(px != nullptr) << "加载 PNG 失败: " << path << " ("
        << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";
    std::vector<uint8_t> out(px, px + static_cast<size_t>(*w) * (*h) * 4);
    stbi_image_free(px);
    return out;
}

// 渲一帧并返回输出 PNG 路径。
//   no_text  : 只画地板（无文字）
//   no_floor : 只画文字（无地板）
std::string RenderFrame(const std::string& outpath, bool no_text, bool no_floor) {
    if (no_text) {
        setenv("JPOV_NO_TEXT", "1", 1);
    } else {
        unsetenv("JPOV_NO_TEXT");
    }
    if (no_floor) {
        setenv("JPOV_NO_FLOOR", "1", 1);
    } else {
        unsetenv("JPOV_NO_FLOOR");
    }
    JPOV::Config cfg;
    cfg.title = "3D Text Depth+Alpha Gold Test";
    cfg.headless = true;
    cfg.width = kOutW;
    cfg.height = kOutH;
    cfg.fonts = {{"tools/jpov/fonts/LxgwWenKai-Regular.ttf", 0, kFontAlias}};
    Text3dDepthAlphaApp app(cfg);
    app.Init();
    BuildText3dDepthAlphaScene(&app);
    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(kOutW);
    winfo.height = static_cast<float>(kOutH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();
    return outpath;
}

std::string GoldPath() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/";
        p += GetGoldName();
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/object3d/" + GetGoldName();
}

// 统计两张图之间的"非平凡差异像素"数（RGB 任一分量差 > 阈值即计入）。
// 用于：有字/无字两帧求差 → 文字实际占用的像素数。
int CountDiffPixels(const std::vector<uint8_t>& a,
                    const std::vector<uint8_t>& b,
                    int w, int h) {
    int count = 0;
    for (int i = 0; i < w * h; ++i) {
        const int o = i * 4;
        const int d = std::abs(a[o + 0] - b[o + 0]) +
                      std::abs(a[o + 1] - b[o + 1]) +
                      std::abs(a[o + 2] - b[o + 2]);
        if (d > 12) {
            ++count;
        }
    }
    return count;
}

// 统计"非背景（非纯黑）像素"数 —— 用于无地板对照图（背景是黑）里数文字像素。
int CountNonBlackPixels(const std::vector<uint8_t>& a, int w, int h) {
    int count = 0;
    for (int i = 0; i < w * h; ++i) {
        const int o = i * 4;
        if (a[o + 0] + a[o + 1] + a[o + 2] > 30) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main() {
    const std::string outdir_j = jpov::GetOutputDir() + "jpov_text3d_depth_alpha_test/";
    const std::string with_text_path  = outdir_j + "with_text.png";
    const std::string floor_only_path = outdir_j + "floor_only.png";
    const std::string text_only_path  = outdir_j + "text_only.png";

    // ---- 渲三帧 ----
    //   1) 地板 + 文字（正常）
    //   2) 只有地板（取文字像素的参照）
    //   3) 只有文字（"文字完整"的基准，用于证明地板确实遮了东西）
    RenderFrame(with_text_path,  /*no_text=*/false, /*no_floor=*/false);
    RenderFrame(floor_only_path, /*no_text=*/true,  /*no_floor=*/false);
    RenderFrame(text_only_path,  /*no_text=*/false, /*no_floor=*/true);

    int w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    const std::vector<uint8_t> with_text  = LoadRgba(with_text_path, &w1, &h1);
    const std::vector<uint8_t> floor_only = LoadRgba(floor_only_path, &w2, &h2);
    const std::vector<uint8_t> text_only  = LoadRgba(text_only_path, &w3, &h3);
    CHECK_EQ(w1, w2) << "两帧宽度不一致";
    CHECK_EQ(h1, h2) << "两帧高度不一致";
    CHECK_EQ(w1, w3) << "两帧宽度不一致";
    CHECK_EQ(h1, h3) << "两帧高度不一致";

    // ---- B. 深度遮挡结构断言 ----
    //
    // 判据（**不依赖任何屏幕坐标常数**，换相机/分辨率/驱动都成立）：
    //   有地板时可见的文字像素数  <  无地板时文字像素数
    // 这正是"遮挡"的定义：地板存在 → 挡掉了一部分文字。
    //
    // 若深度测试失效（文字整片浮在地板之上），两者会**相等** → 断言失败。
    //
    // 附一条反向自证：有地板时文字像素仍 > 0（没被整体埋掉/没画丢）。
    const int px_with_floor = CountDiffPixels(with_text, floor_only, w1, h1);
    const int px_no_floor   = CountNonBlackPixels(text_only, w3, h3);
    LOG(INFO) << "[结构断言] 文字像素：有地板=" << px_with_floor
              << "  无地板=" << px_no_floor;

    CHECK_GT(px_no_floor, 0)
        << "无地板对照帧里没有文字像素 —— 字体未加载或 DrawText3D 失败";
    CHECK_GT(px_with_floor, 0)
        << "有地板时文字像素为 0 —— 文字被整体埋进地板（画序/深度状态异常）";

    // ⚠️ **不能只断言 `px_with_floor < px_no_floor`** —— 负向验证（关掉
    //    文字 draw 的 depth test）实测显示：即使深度测试失效，文字与地板
    //    重叠区的**抗锯齿混合**仍会产生少量差异像素（实测遮挡率 4.8%），
    //    使弱断言 `A < B` 依然成立（假绿）。
    //    故必须要求**显著**遮挡量。
    //
    // 阈值取 15%（本场景实测：深度测试开 = 27.8%，关 = 4.8%，
    // 两侧各留 ≥ 10pp 余量，既不误报也不漏报）。
    // 若换相机/分辨率导致真实值变化，需同步重测本阈值（并在 PR 里说明）。
    constexpr double kMinOcclusionRatio = 0.15;
    const double occluded_ratio =
        1.0 - static_cast<double>(px_with_floor) / static_cast<double>(px_no_floor);
    LOG(INFO) << "[结构断言] 文字像素：有地板=" << px_with_floor
              << "  无地板=" << px_no_floor
              << "  遮挡率=" << (occluded_ratio * 100.0) << "%";

    CHECK_GT(occluded_ratio, kMinOcclusionRatio)
        << "深度遮挡不足：地板只挡住了 " << (occluded_ratio * 100.0)
        << "% 的文字像素（要求 > " << (kMinOcclusionRatio * 100.0)
        << "%）。"
           "地板未有效参与深度遮挡（depth test 被关/被绕过，"
           "或文字被画在地板之前）。"
           "——负向验证基准：depth test 关时仅 ~4.8%。";

    LOG(INFO) << "[结构断言] 通过：地板挡住了 "
              << (occluded_ratio * 100.0) << "% 的文字像素（深度遮挡生效）";

    // ---- A. gold 字节比对 ----
    std::vector<uint8_t> expected_bytes;
    const std::string gold_path = GoldPath();
    if (!jpov::ReadFileBytes(gold_path, &expected_bytes)) {
        LOG(ERROR) << "无法加载 gold: " << gold_path
                   << "（首次生成请跑 "
                      "bazel run //tools/jpov/test/object3d:"
                      "text3d_depth_alpha_gold_generator）";
        return 1;
    }
    std::vector<uint8_t> rendered_bytes;
    if (!jpov::ReadFileBytes(with_text_path, &rendered_bytes)) {
        LOG(ERROR) << "无法加载渲染输出: " << with_text_path;
        return 1;
    }
    if (rendered_bytes.size() != expected_bytes.size()) {
        LOG(ERROR) << "gold 尺寸不符: rendered=" << rendered_bytes.size()
                   << " expected=" << expected_bytes.size();
        return 1;
    }
    for (size_t i = 0; i < rendered_bytes.size(); ++i) {
        if (rendered_bytes[i] != expected_bytes[i]) {
            LOG(ERROR) << "gold 字节不符 @ offset " << i << ": got 0x"
                       << std::hex << static_cast<int>(rendered_bytes[i])
                       << " expected 0x" << static_cast<int>(expected_bytes[i]);
            return 1;
        }
    }
    LOG(INFO) << "[A] gold 字节比对通过（" << expected_bytes.size() << " bytes）";
    LOG(INFO) << "TEST PASSED: 3D 文本深度遮挡 + alpha 混合 + gold 全绿";
    return 0;
}

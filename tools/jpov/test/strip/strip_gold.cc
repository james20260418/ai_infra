// JPOV 半透明条带 Gold Test —— M1（覆盖集 + 一次 draw call）
//
// 固定场景（天光 + 地面 + 三块参考方块 + 一批三条带）、固定相机、1280×720，
// 与 git 管理的 PNG 做**逐字节**比较。
//
// 用法：
//   bazel run //tools/jpov/test/strip:strip_gold_generator   # 重出 gold
//   bazel test //tools/jpov/test/strip:strip_gold_test       # 回归
//
// 除逐字节之外的三组**非恒真**断言（防止「参数退化 + 重出 gold」一起绿）：
//   ① 纹理路径真的生效：带纹理 vs 纯色的差异像素数 ∈ [20000, 120000]；
//   ② 纹理的色带真的投影到画面上：在①的差异区域内，
//      红/青/黄三种「主色像素」各自数量落在区间内（纹理没绑上/UV 退化都会掉到 0）；
//   ③ 宽度剖面真的进片元：柔边 vs 硬边的差异像素数 ∈ [5000, 80000]。
//
// 「一个 draw call」不在本测试断言（像素不可测）：证据是 StripRenderer 每次 Draw
// 打一条 `strips=N draw_calls=M` 日志（本测试跑起来时 N=3, M=1）。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/demo/strip_viewer_app.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/test_utils.h"
#include "third_party/stb/stb_image.h"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// 条纹素材（tools/jpov/assets/strip/ribbon_test.png）。
std::string StripAssetPath() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    const std::string root =
        srcdir != nullptr ? (std::string(srcdir) + "/__main__/") : jpov::GetProjectRoot();
    return root + "tools/jpov/assets/strip/ribbon_test.png";
}

std::string GoldPath() {
    return jpov::GetTestDataDir() + "/strip/strip_demo_1280x720.png";
}

// 三种拍摄模式（A/B 用）。
enum class Shot { kTextured, kSolid, kHardEdges };

void RenderTo(const std::string& out_path, Shot shot) {
    JPOV::Config cfg;
    cfg.title = "JPOV strip gold test";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    jpov_strip::StripViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.Setup(StripAssetPath());
    jpov_strip::ApplyDefaultView(&app);
    app.use_texture_ = (shot != Shot::kSolid);
    app.soft_edges_ = (shot != Shot::kHardEdges);

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb At(const unsigned char* img, int width, int x, int y) {
    const int i = (y * width + x) * 3;
    return Rgb{img[i], img[i + 1], img[i + 2]};
}

// 读两张图，统计「RGB 任一分量差 > 1」的像素数；返回 -1 表示解码/尺寸失败。
long CountDifferingPixels(const std::string& a_path, const std::string& b_path) {
    int aw = 0;
    int ah = 0;
    int ac = 0;
    int bw = 0;
    int bh = 0;
    int bc = 0;
    unsigned char* a = stbi_load(a_path.c_str(), &aw, &ah, &ac, 3);
    unsigned char* b = stbi_load(b_path.c_str(), &bw, &bh, &bc, 3);
    if (a == nullptr || b == nullptr || aw != bw || ah != bh) {
        stbi_image_free(a);
        stbi_image_free(b);
        return -1;
    }
    long count = 0;
    for (int i = 0; i < aw * ah; ++i) {
        const int d = std::max(std::abs(int(a[i * 3 + 0]) - int(b[i * 3 + 0])),
                               std::max(std::abs(int(a[i * 3 + 1]) - int(b[i * 3 + 1])),
                                        std::abs(int(a[i * 3 + 2]) - int(b[i * 3 + 2]))));
        if (d > 1) {
            ++count;
        }
    }
    stbi_image_free(a);
    stbi_image_free(b);
    return count;
}

// 统计「纹理 vs 纯色」差异区域内满足颜色判据的像素数。
// 判据（都在 0..255 域）：
//   kRed    : r > g+25 且 r > b+25
//   kCyan   : b > r+40 且 g > r+25
//   kYellow : r > b+50 且 g > b+50
enum class Hue { kRed, kCyan, kYellow };

long CountHueInsideTextureRegion(const std::string& textured_path,
                                 const std::string& solid_path, Hue hue) {
    int aw = 0;
    int ah = 0;
    int ac = 0;
    int bw = 0;
    int bh = 0;
    int bc = 0;
    unsigned char* a = stbi_load(textured_path.c_str(), &aw, &ah, &ac, 3);
    unsigned char* b = stbi_load(solid_path.c_str(), &bw, &bh, &bc, 3);
    if (a == nullptr || b == nullptr || aw != bw || ah != bh) {
        stbi_image_free(a);
        stbi_image_free(b);
        return -1;
    }
    long count = 0;
    for (int y = 0; y < ah; ++y) {
        for (int x = 0; x < aw; ++x) {
            const Rgb pa = At(a, aw, x, y);
            const Rgb pb = At(b, aw, x, y);
            const int diff = std::max(std::abs(pa.r - pb.r),
                                      std::max(std::abs(pa.g - pb.g),
                                               std::abs(pa.b - pb.b)));
            if (diff <= 2) {
                continue;   // 不在「纹理改变了这个像素」的区域内
            }
            bool hit = false;
            if (hue == Hue::kRed) {
                hit = pa.r > pa.g + 25 && pa.r > pa.b + 25;
            } else if (hue == Hue::kCyan) {
                hit = pa.b > pa.r + 40 && pa.g > pa.r + 25;
            } else {
                hit = pa.r > pa.b + 50 && pa.g > pa.b + 50;
            }
            if (hit) {
                ++count;
            }
        }
    }
    stbi_image_free(a);
    stbi_image_free(b);
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    const bool generate = (argc > 1 && std::strcmp(argv[1], "--generate") == 0);
    if (generate) {
        std::filesystem::create_directories(
            std::filesystem::path(GoldPath()).parent_path());
        RenderTo(GoldPath(), Shot::kTextured);
        LOG(INFO) << "gold written: " << GoldPath();
        return 0;
    }

    std::vector<uint8_t> expected;
    if (!jpov::ReadFileBytes(GoldPath(), &expected)) {
        LOG(FATAL) << "missing gold: " << GoldPath();
    }

    // 主 gold（带纹理 + 柔边）到临时文件后逐字节比较。
    const std::string tmp_dir = "/tmp/jpov_strip_gold";
    std::filesystem::create_directories(tmp_dir);
    const std::string actual = tmp_dir + "/strip_demo.png";
    const std::string solid = tmp_dir + "/strip_solid.png";
    const std::string hard = tmp_dir + "/strip_hard.png";
    RenderTo(actual, Shot::kTextured);
    RenderTo(solid, Shot::kSolid);
    RenderTo(hard, Shot::kHardEdges);

    std::vector<uint8_t> actual_bytes;
    if (!jpov::ReadFileBytes(actual, &actual_bytes)) {
        LOG(FATAL) << "missing actual: " << actual;
    }
    CHECK_EQ(actual_bytes.size(), expected.size())
        << "gold 尺寸不一致：" << actual_bytes.size() << " vs " << expected.size();
    CHECK(actual_bytes == expected) << "gold 逐字节不一致: " << actual;

    // ① 纹理路径生效。
    const long textured_diff = CountDifferingPixels(actual, solid);
    CHECK_GE(textured_diff, 20000) << "带纹理与纯色几乎一致 → 纹理可能没绑上";
    CHECK_LE(textured_diff, 120000) << "带纹理与纯色差异过大 → 画面可能整体被改";
    LOG(INFO) << "[assert] textured-vs-solid diff px = " << textured_diff;

    // ② 三条色带都真的投影到画面上（红 / 青 / 黄各占一片）。
    const long red = CountHueInsideTextureRegion(actual, solid, Hue::kRed);
    const long cyan = CountHueInsideTextureRegion(actual, solid, Hue::kCyan);
    const long yellow = CountHueInsideTextureRegion(actual, solid, Hue::kYellow);
    CHECK_GE(red, 3000) << "差异区域内几乎没有红色主色像素 → 红色带缺失";
    CHECK_LE(red, 40000);
    CHECK_GE(cyan, 3000) << "差异区域内几乎没有青色主色像素 → 青色带缺失";
    CHECK_LE(cyan, 40000);
    CHECK_GE(yellow, 1000) << "差异区域内几乎没有黄色主色像素 → 黄色带缺失";
    CHECK_LE(yellow, 30000);
    LOG(INFO) << "[assert] hue px red=" << red << " cyan=" << cyan << " yellow=" << yellow;

    // ③ 宽度剖面真的进片元（柔边 vs 硬边必须显著不同）。
    const long profile_diff = CountDifferingPixels(actual, hard);
    CHECK_GE(profile_diff, 5000) << "柔边/硬边几乎一样 → width_profile 可能没生效";
    CHECK_LE(profile_diff, 80000) << "柔边/硬边差异过大 → 剖面可能溢出";
    LOG(INFO) << "[assert] soft-vs-hard diff px = " << profile_diff;

    LOG(INFO) << "strip gold test passed";
    return 0;
}

// JPOV debug HUD GPU 像素测试 —— 锁住「按 ~ 的左上角 HUD」真的画出来了
//
// 这个测试针对的核心风险是「静默什么都不画」：
//   * HUD 用**内置点阵字体**，不走 FontRenderer；如果没有字体就静默 skip
//     （FontRenderer 的行为），那 HUD 会在所有 app 上都消失而不报错。
//   * HUD 在 Render() 之后画，若 FBO 绑定/着色器状态不对，也会静默画到别处。
// 两类问题 CPU 层都看不见，只能读回像素。
//
// 场景刻意做得极简：**一个字体都不注册**（`cfg.fonts` 保持空）——这正是
// 内置点阵字体要解决的问题。app 只画一块纯色矩形，方便断言「HUD 之外一个像素没动」。
//
// 断言清单：
//   1. 关掉 HUD：整个 top band 内**零个纯白像素**（场景底色是 0.2/0.3/0.4）
//   2. 打开 HUD：top band 内出现大量纯白像素，且最左上那个白像素落在 (8,8) 附近
//      ⇒ 位置是左上角，不是别处
//   3. HUD 之外的区域（y >= 150）与关闭时**逐字节相同** ⇒ 不污染场景/不漏状态
//   4. 改相机机位后文本变化 ⇒ band 内像素跟着变（HUD 真的反映 DebugInfo 状态）
//   5. 再关掉 ⇒ 与最初的关闭图像**逐字节相同** ⇒ 无残留

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/debug_overlay/debug_font8x8.h"

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;

// HUD 的文本区上界（6~7 行 x 11px/行 + 8px 上边距 ≈ 85px，留足余量取 150）。
constexpr int kBandBottom = 150;

// 场景底色（与 HUD 的纯白形成明确区分）。
constexpr float kSceneR = 0.2f;
constexpr float kSceneG = 0.3f;
constexpr float kSceneB = 0.4f;

struct Image {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> px;   // RGBA，行 0 = 图像顶部
};

Image LoadRgba(const std::string& path) {
    Image img;
    int comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &img.w, &img.h, &comp, 4);
    CHECK(data != nullptr) << "读不回渲染结果：" << path;
    img.px.assign(data, data + static_cast<std::size_t>(img.w) * img.h * 4);
    stbi_image_free(data);
    return img;
}

// 纯白像素（点阵是二值的：要么 255 全亮，要么背景色）。
bool IsPureWhite(const Image& img, int x, int y) {
    const std::size_t o = static_cast<std::size_t>(y) * img.w * 4 + x * 4;
    return img.px[o] > 250 && img.px[o + 1] > 250 && img.px[o + 2] > 250;
}

int CountWhiteInBand(const Image& img, int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < img.w; ++x) {
            if (IsPureWhite(img, x, y)) {
                ++n;
            }
        }
    }
    return n;
}

// 区域内逐字节不同的像素数。
int CountDiffInBand(const Image& a, const Image& b, int y0, int y1) {
    CHECK_EQ(a.w, b.w);
    CHECK_EQ(a.h, b.h);
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < a.w; ++x) {
            const std::size_t o = static_cast<std::size_t>(y) * a.w * 4 + x * 4;
            if (a.px[o + 0] != b.px[o + 0] || a.px[o + 1] != b.px[o + 1] ||
                a.px[o + 2] != b.px[o + 2] || a.px[o + 3] != b.px[o + 3]) {
                ++n;
            }
        }
    }
    return n;
}

// 找 band 内纯白像素的左上角（x 最小 / y 最小），用于断言 HUD 在左上角。
bool FindFirstWhite(const Image& img, int y0, int y1, int* out_min_x, int* out_min_y) {
    int min_x = img.w;
    int min_y = img.h;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < img.w; ++x) {
            if (IsPureWhite(img, x, y)) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
            }
        }
    }
    if (min_x == img.w) {
        return false;
    }
    *out_min_x = min_x;
    *out_min_y = min_y;
    return true;
}

// 读一个 8x8 字形格子的点阵（行 0 = 顶部，bit i = 该行第 i 列像素）。
// scale = 字形放大倍数：每个点阵像素占 scale x scale 个屏幕像素，取块中心采样。
std::array<uint8_t, 8> ReadCellBits(const Image& img, int x0, int y0, int scale) {
    std::array<uint8_t, 8> bits{};
    const int half = scale / 2;
    for (int row = 0; row < 8; ++row) {
        uint8_t v = 0;
        for (int col = 0; col < 8; ++col) {
            if (IsPureWhite(img, x0 + col * scale + half, y0 + row * scale + half)) {
                v = static_cast<uint8_t>(v | (1u << col));
            }
        }
        bits[row] = v;
    }
    return bits;
}

// 把 HUD 区域逐字与**期望文本**对校对：期望文本的每个字符在它的格子上必须
// 与该字模的点阵**逐位相同**。
//
// 这样一条断言同时钉死了四件容易错的事：字形下标、字符前进量、行高、图集 v 方向
// （反了会整体上下翻转）。比“有白像素”强得多。返回第一个不匹配的描述（空 = 全对）。
std::string VerifyOverlayText(const Image& img, const std::string& expected,
                              int scale) {
    // 按 '\n' 切行。
    std::vector<std::string> lines;
    std::string cur;
    for (const char ch : expected) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        cur += ch;
    }
    lines.push_back(cur);

    constexpr int kPad = 8;        // 与 DebugOverlayRenderer::kPadPx 一致（不随 scale 变）
    constexpr int kLineGap = 3;    // 与 DebugOverlayRenderer::kLineGapPx 一致（不随 scale 变）
    constexpr int kGlyph = 8;      // 与 kDebugFontGlyphW/H 一致
    const int advance = kGlyph * scale;
    const int line_h = (kGlyph + kLineGap) * scale;

    for (std::size_t L = 0; L < lines.size(); ++L) {
        const std::string& line = lines[L];
        if (static_cast<int>(line.size()) * advance + kPad > img.w) {
            return "期望文本第 " + std::to_string(L) + " 行超出画面宽度（" +
                   std::to_string(line.size()) + " 字符）—— 测试场景需要更宽的窗口";
        }
        for (std::size_t C = 0; C < line.size(); ++C) {
            const unsigned char code = static_cast<unsigned char>(line[C]);
            if (code < jpov::kDebugFontFirstChar || code > jpov::kDebugFontLastChar) {
                continue;   // 区间外字符按设计画成空白，不比对
            }
            const std::array<uint8_t, 8> got =
                ReadCellBits(img, kPad + static_cast<int>(C) * advance,
                             kPad + static_cast<int>(L) * line_h, scale);
            const uint8_t* want = jpov::kDebugFont8x8[code - jpov::kDebugFontFirstChar];
            for (int row = 0; row < 8; ++row) {
                if (got[row] != want[row]) {
                    return "字符不匹配：行 " + std::to_string(L) + " 列 " +
                           std::to_string(C) + "（字符 '" + line[C] + "'，row " +
                           std::to_string(row) + "）：期望 0x" +
                           std::to_string(want[row]) + " 实际 0x" +
                           std::to_string(got[row]);
                }
            }
        }
    }
    return std::string();
}

// 极简 app：**不注册任何字体**，只画一块纯色矩形 + 用成员相机。
class OverlayTestApp : public JPOV {
public:
    explicit OverlayTestApp(Config cfg) : JPOV(cfg) {}

    // 相机机位（测试会改它，验证 HUD 跟随状态变化）。
    jpov::Camera cam;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        cmds->camera = cam;
        cmds->DrawRect({0.0f, 0.0f}, {winfo.width, winfo.height},
                       jpov::Color{kSceneR, kSceneG, kSceneB, 1.0f});
    }
};

std::string Shot(const std::string& outdir, const char* name) {
    return outdir + name;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_debug_overlay_test/";
    const std::string base_path = Shot(outdir, "overlay_off.png");
    const std::string on1_path  = Shot(outdir, "overlay_on_cam_a.png");
    const std::string on2_path  = Shot(outdir, "overlay_on_cam_b.png");
    const std::string off2_path = Shot(outdir, "overlay_off_again.png");

    JPOV::Config cfg;
    cfg.title = "Debug Overlay Test";
    cfg.headless = true;
    // cfg.fonts 刻意留空：**一个字体都不注册**。内置点阵字体必须照样能画。
    CHECK(cfg.fonts.empty());

    OverlayTestApp app(cfg);
    app.Init();
    // 先证明引擎确实采到了 GPU 信息（kUnknown = 查询失败，那后面的“零字体”也没意义）。
    CHECK(app.debug().gpu.kind != jpov::GpuKind::kUnknown)
        << "Init() 之后应该已经采到 GPU 信息（实际 kind = "
        << jpov::GpuKindName(app.debug().gpu.kind) << "）";

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};

    // ---- 1. 关闭 HUD 的基准图 ----
    app.cam.position = {1.25f, -2.5f, 3.0f};
    app.cam.target = {0.0f, 0.0f, 0.0f};
    app.SetDebugOverlayVisible(false);
    app.RunOnce(/*frame_count=*/0, input, winfo, base_path.c_str());

    const Image base = LoadRgba(base_path);
    CHECK_EQ(base.w, kWidth);
    CHECK_EQ(base.h, kHeight);
    CHECK_EQ(CountWhiteInBand(base, 0, kBandBottom), 0)
        << "关掉 HUD 时不该有任何纯白像素（场景底色是 0.2/0.3/0.4）";
    LOG(INFO) << "[PASS] 关闭态：HUD 区域零纯白像素";

    // 场景真的画了（底色矩形在）：取画面中心像素。
    {
        const std::size_t o =
            static_cast<std::size_t>(kHeight / 2) * kWidth * 4 + (kWidth / 2) * 4;
        CHECK_NEAR(base.px[o + 0], static_cast<unsigned char>(std::lround(0.2f * 255.0f)), 2);
        CHECK_NEAR(base.px[o + 1], static_cast<unsigned char>(std::lround(0.3f * 255.0f)), 2);
        CHECK_NEAR(base.px[o + 2], static_cast<unsigned char>(std::lround(0.4f * 255.0f)), 2);
    }
    LOG(INFO) << "[PASS] 场景底色矩形已渲染（中心像素 = 0.2/0.3/0.4）";

    // ---- 2. 打开 HUD：出现文字，且位置在左上角 ----
    app.SetDebugOverlayVisible(true);
    app.RunOnce(/*frame_count=*/1, input, winfo, on1_path.c_str());
    const Image on1 = LoadRgba(on1_path);

    const int white_on1 = CountWhiteInBand(on1, 0, kBandBottom);
    CHECK_GT(white_on1, 200) << "HUD 开着却几乎没画出白色文字（实际 = " << white_on1
                             << " 个纯白像素）";
    LOG(INFO) << "[PASS] 打开态：band 内纯白像素 = " << white_on1;

    int min_x = 0;
    int min_y = 0;
    CHECK(FindFirstWhite(on1, 0, kBandBottom, &min_x, &min_y));
    CHECK_GE(min_x, 8) << "文字不该贴到左边缘（pad=8）";
    CHECK_LT(min_x, 40) << "第一个字应在最左侧（x=" << min_x << "）";
    CHECK_GE(min_y, 8) << "文字不该贴到上边缘（pad=8）";
    CHECK_LT(min_y, 30) << "第一行应在最上方（y=" << min_y << "）";
    LOG(INFO) << "[PASS] HUD 在左上角：首个白像素 = (" << min_x << ", " << min_y << ")";

    // ---- 2b. 逐字比对：渲染出来的字形必须与 DebugString() 完全一致 ----
    // 这是最硬的一条：同时钉死字形下标、前进量、行高、图集 v 方向。
    const std::string expected_text = app.debug().DebugString();
    const std::string mismatch = VerifyOverlayText(on1, expected_text, 1);
    CHECK(mismatch.empty()) << mismatch << "\n期望文本：\n" << expected_text;
    LOG(INFO) << "[PASS] 逐字比对通过（" << expected_text.size() << " 字符）";

    // ---- 3. HUD 之外一个像素都不能动 ----
    const int diff_outside = CountDiffInBand(base, on1, kBandBottom, kHeight);
    CHECK_EQ(diff_outside, 0) << "HUD 区域之外有 " << diff_outside
                              << " 个像素被改动（说明 overlay 漏到了全屏）";
    LOG(INFO) << "[PASS] HUD 之外逐字节相同";

    // ---- 4. 改相机 ⇒ 文本跟着变 ----
    app.cam.position = {-7.75f, 3.25f, 0.5f};
    app.RunOnce(/*frame_count=*/2, input, winfo, on2_path.c_str());
    const Image on2 = LoadRgba(on2_path);

    const int diff_cam = CountDiffInBand(on1, on2, 0, kBandBottom);
    CHECK_GT(diff_cam, 50) << "相机机位变了但 HUD 像素几乎没变（实际 = " << diff_cam
                           << "）—— HUD 没反映 DebugInfo 状态";
    CHECK_EQ(CountDiffInBand(on1, on2, kBandBottom, kHeight), 0)
        << "改相机不该影响 HUD 之外的像素";
    LOG(INFO) << "[PASS] 相机机位进 HUD：band 内变化像素 = " << diff_cam;

    // ---- 5. 再关掉：与最初完全一致（无残留）----
    app.SetDebugOverlayVisible(false);
    app.RunOnce(/*frame_count=*/3, input, winfo, off2_path.c_str());
    const Image off2 = LoadRgba(off2_path);

    CHECK_EQ(CountDiffInBand(base, off2, 0, kHeight), 0)
        << "第二次关掉 HUD 后与最初的开局画面不一致（有残留）";
    LOG(INFO) << "[PASS] 再次关闭：与基准图逐字节相同";

    app.Finalize();

    // ---- 6. glyph_scale = 2：整数字节放大后，每个点阵像素应占 2x2 屏幕像素 ----
    // 换个 app 实例（debug_overlay_scale 是 Config，构造时定）；顺便验证
    // 「Finalize() 之后同一进程可以再次 Init()」这条文档口头承诺确实成立。
    {
        JPOV::Config cfg2 = cfg;
        cfg2.debug_overlay = true;          // 请求"常驻"
        cfg2.debug_overlay_scale = 2;
        OverlayTestApp app2(cfg2);
        app2.Init();
        // headless ⇒ 初值强制 false（见 Config::debug_overlay 文档）：
        // 否则所有 headless gold 截图都会被这 9 行 HUD 污染。要看得显式打开。
        CHECK(!app2.is_debug_overlay_visible())
            << "headless 下 debug_overlay 初值必须为 false";
        LOG(INFO) << "[PASS] headless 下 HUD 初值关闭（gold 截图不会被污染）";
        app2.SetDebugOverlayVisible(true);
        // 放大 2 倍后最长一行 61 字符 x 16px = 976px ⇒ 窗口要比前面宽，
        // 否则文字会被视口裁掉（那是设计行为，但会让比对无法进行）。
        jpov::WindowInfo winfo_big;
        winfo_big.width = 1600.0f;
        winfo_big.height = 520.0f;
        const std::string scaled_path = Shot(outdir, "overlay_scale2.png");
        app2.RunOnce(/*frame_count=*/0, input, winfo_big, scaled_path.c_str());
        const Image scaled = LoadRgba(scaled_path);

        const std::string mismatch2 =
            VerifyOverlayText(scaled, app2.debug().DebugString(), 2);
        CHECK(mismatch2.empty()) << mismatch2 << "\n期望文本：\n"
                                 << app2.debug().DebugString();
        LOG(INFO) << "[PASS] glyph_scale=2 逐字比对通过（放大后仍逐位正确）";
        app2.Finalize();
    }
    LOG(INFO) << "全部断言通过。";
    return 0;
}

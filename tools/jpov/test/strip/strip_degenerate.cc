// JPOV 半透明条带 —— **退化输入**的 GPU 端探针（headless，1280×720）
//
// 起因（Danis）：「万一 JPOV 用户输入了奇异的数值，JPOV 该不会崩溃吧？NaN/Inf 可以约定
// 用户不会犯傻（render command proto 化时能证据实锤），但**像重叠点这类的输入**，会不会
// 让 renderer 或者内部除以 0？」
//
// 本测试喂的是「**合法但退化**」的几何：全部点重合、零宽、连续两点重合、中间收到 0 宽、
// 亚微米段长、180° 折返、坐标 1e6 米、只给 2 个点且两点重合…… 这些都能通过
// ValidateStripBatch（它们不是错误输入，只是几何退化了），所以必须**画得出来、且不害别人**。
//
// 三类断言（都是可证伪的，不是恒真）：
//   ① **零面积退化**（全重合 / 零宽 / 两点重合）画上去必须**一个像素都不改** ——
//      对照图是同机位、同场景、只是不发那批条带；
//   ② **有面积的退化**（沙漏 / 折返 / 亚微米 / 远坐标）与一条正常条带同批渲染时，
//      条带区域**之外**的像素必须与对照图逐像素相同 —— 这是 NaN 的探测器：
//      任何一个 NaN 写进 1× WBO 都会在 composite 里铺满全屏（画面变黑/变花），
//      只要铺开了，条带区域外就不可能还是逐像素相同；
//   ③ 同批里的那条**正常条带仍然可见**（差异像素数落在区间内）——
//      证明退化邻居既没吃掉它、也没让整批被丢弃。
//
// 为什么用「对照图 + 像素差」而不是直接查 NaN：最终产物是 8bit PNG，NaN 落盘后
// 变成 0 或垃圾值，看不出「这是 NaN」；而「污染会不会扩散」正是真正想知道的事。
//
// 运行：bazel test //tools/jpov/test/strip:strip_degenerate_test

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

std::string StripAssetPath() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    const std::string root =
        srcdir != nullptr ? (std::string(srcdir) + "/__main__/") : jpov::GetProjectRoot();
    return root + "tools/jpov/assets/strip/ribbon_test.png";
}

std::string TmpPath(const char* name) {
    return std::string("/tmp/jpov_strip_degenerate/") + name + ".png";
}

// 场景：viewer 的地面 + 参考方块 + 天光 + 默认机位；**不发 viewer 自带的条带**，
// 由本测试自己发「退化条带」的批。
class DegenerateApp : public jpov_strip::StripViewerApp {
public:
    using StripViewerApp::StripViewerApp;

    // 本帧要发的批（互斥，便于 A/B）：none / zero-area / area / area+normal
    enum class Mode {
        kNone = 0,
        kZeroArea = 1,
        kArea = 2,
        kAreaPlusNormal = 3,
    };
    Mode mode = Mode::kNone;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        StripViewerApp::OneIteration(frame_count, input, winfo, cmds);
        if (mode == Mode::kNone) {
            return;
        }
        jpov::TranslucentStrip16Batch batch;
        batch.texture_id = 0;   // 纯色：退化几何不靠纹理也能看清
        batch.geometry = jpov::StripInterpolation::kSmooth;   // 平滑 pass 的除法最多，先压它
        if (mode == Mode::kZeroArea) {
            AppendZeroArea(&batch);
        } else {
            AppendArea(&batch);
        }
        if (mode == Mode::kAreaPlusNormal) {
            // 同批里的「好公民」：一条正常宽度的直条带。
            batch.strips.push_back(MakeStraight(/*y=*/0.1f, /*half=*/0.18f));
        }
        cmds->DrawStrip16Batch(std::move(batch));
    }

private:
    // 一条落在相机前方的直条带：x ∈ [-2, 2]，y 由参数给，半宽 half。
    static jpov::Strip16 MakeStraight(float y, float half) {
        jpov::Strip16 s;
        for (int i = 0; i < jpov::kStripSampleCount; ++i) {
            const float x = -2.0f + 4.0f * static_cast<float>(i) /
                                        static_cast<float>(jpov::kStripSampleCount - 1);
            s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, y + half, -0.5f};
            s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, y - half, -0.5f};
        }
        s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
        return s;
    }

    // ① 零面积退化：这些条带在屏幕上不该产生**任何**片元。
    static void AppendZeroArea(jpov::TranslucentStrip16Batch* batch) {
        // (a) 全部点重合（+ 全宽）：每个 quad 长度 0 ⇒ 零面积。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{0.0f, 1.2f, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{0.0f, 1.0f, -0.5f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (b) 零宽（left == right）：所有 quad 宽度 0 ⇒ 零面积。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const float x = -2.0f + 4.0f * static_cast<float>(i) /
                                            static_cast<float>(jpov::kStripSampleCount - 1);
                const jpov::Vec3f p{x, 1.2f, -0.5f};
                s.left[static_cast<size_t>(i)] = p;
                s.right[static_cast<size_t>(i)] = p;
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (c) 只给 2 个点、且两点完全重合：塌缩成 30 个零面积 quad。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{0.5f, 1.2f, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{0.5f, 1.0f, -0.5f};
            }
            s.point_count = 2;
            batch->strips.push_back(s);
        }
    }

    // ② 有面积、但几何退化的条带：允许出现在画面上，但不许伤害别人。
    static void AppendArea(jpov::TranslucentStrip16Batch* batch) {
        // (a) 沙漏：中间一列宽度收到 0（两侧仍有宽度）。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const float x = -3.0f + 2.0f * static_cast<float>(i) /
                                            static_cast<float>(jpov::kStripSampleCount - 1);
                const float half = (i == jpov::kStripSampleCount / 2) ? 0.0f : 0.25f;
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, 2.2f + half, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, 2.2f - half, -0.5f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (b) 180° 折返：后半段沿着前半段走回去（切线在折返点退化为 0 附近）。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const int k = (i < jpov::kStripSampleCount / 2)
                                  ? i
                                  : (jpov::kStripSampleCount - 1 - i);
                const float x = -3.0f + 2.0f * static_cast<float>(k) /
                                            static_cast<float>(jpov::kStripSampleCount / 2);
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, 1.8f + 0.15f, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, 1.8f - 0.15f, -0.5f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (c) 亚微米段长（整条 1.5e-6 m）：曲线外扩量算不出来，但不许出 NaN。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const float x = 1e-7f * static_cast<float>(i);
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, 2.6f + 1e-7f, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, 2.6f - 1e-7f, -0.5f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (d) **楔形**：第一列宽度为 0、往后张开。尖端的片元满足 Pv = 2·Hh ≈ 0，
        //     而 FS 里 dv = (D·Pv)/(Pv·Pv) 正是 0/0 的形状 —— 这条用例专门压那个除法
        //     （去掉 max(Pv·Pv, 1e-8) 守卫后它会红，见 PR 里的负向对照）。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const float t = static_cast<float>(i) /
                                static_cast<float>(jpov::kStripSampleCount - 1);
                const float x = -3.0f + 3.0f * t;
                const float half = 0.30f * t;   // t=0 时半宽恰为 0
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, 1.4f + half, -0.5f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, 1.4f - half, -0.5f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
        // (e) 坐标 1e6 米（远在相机外）—— 顶点变换后仍必须是有限值。
        {
            jpov::Strip16 s;
            for (int i = 0; i < jpov::kStripSampleCount; ++i) {
                const float x = 1e6f + static_cast<float>(i);
                s.left[static_cast<size_t>(i)] = jpov::Vec3f{x, 1e6f + 0.2f, 1e6f};
                s.right[static_cast<size_t>(i)] = jpov::Vec3f{x, 1e6f - 0.2f, 1e6f};
            }
            s.point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
            batch->strips.push_back(s);
        }
    }
};

void RenderTo(const std::string& out_path, DegenerateApp::Mode mode) {
    JPOV::Config cfg;
    cfg.title = "JPOV strip degenerate probe";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    DegenerateApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.Setup(StripAssetPath());
    jpov_strip::ApplyDefaultView(&app);
    // viewer 自带的场景元素全部关掉：只留地面 + 天光（相机/光照与对照图一致）。
    app.show_strips_ = false;
    app.ring_visible_ = false;
    app.jet_visible_ = false;
    app.tank_visible_ = false;
    app.sampling_visible_ = false;
    app.boxes_visible_ = true;
    app.smaa_ = false;
    app.bloom_ = false;
    app.mode = mode;

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

struct DiffStats {
    bool ok = false;
    int max_channel = 255;      // 全图最大通道差
    long differing = 0;         // 全图「任一通道差 > 1」的像素数
    long differing_outside = 0; // 只看条带区域之外
    int outside_max = 255;      // 条带区域之外的最大通道差
};

// 条带可能出现的屏幕范围（放宽一圈，避免「刚好在边界上」的误报）。
// 相机/机位固定（ApplyDefaultView），实测条带落在中间偏上；这里给一个保守的大框。
constexpr int kBoxX0 = 260;
constexpr int kBoxX1 = 1020;
constexpr int kBoxY0 = 120;
constexpr int kBoxY1 = 620;

DiffStats Compare(const std::string& a_path, const std::string& b_path) {
    DiffStats st;
    int aw = 0;
    int ah = 0;
    int ac = 0;
    int bw = 0;
    int bh = 0;
    int bc = 0;
    unsigned char* a = stbi_load(a_path.c_str(), &aw, &ah, &ac, 3);
    unsigned char* b = stbi_load(b_path.c_str(), &bw, &bh, &bc, 3);
    if (a == nullptr || b == nullptr || aw != bw || ah != bh) {
        if (a != nullptr) {
            stbi_image_free(a);
        }
        if (b != nullptr) {
            stbi_image_free(b);
        }
        return st;
    }
    st.max_channel = 0;
    for (int y = 0; y < ah; ++y) {
        for (int x = 0; x < aw; ++x) {
            const int i = (y * aw + x) * 3;
            const int d = std::max({std::abs(a[i] - b[i]), std::abs(a[i + 1] - b[i + 1]),
                                    std::abs(a[i + 2] - b[i + 2])});
            st.max_channel = std::max(st.max_channel, d);
            if (d > 1) {
                ++st.differing;
            }
            const bool inside = (x >= kBoxX0 && x <= kBoxX1 && y >= kBoxY0 && y <= kBoxY1);
            if (!inside) {
                st.outside_max = std::min(st.outside_max, 255);
                if (d > st.outside_max || st.outside_max == 255) {
                    st.outside_max = std::max(st.outside_max == 255 ? 0 : st.outside_max, d);
                }
                if (d > 1) {
                    ++st.differing_outside;
                }
            }
        }
    }
    st.ok = true;
    stbi_image_free(a);
    stbi_image_free(b);
    return st;
}

}  // namespace

int main() {
    std::filesystem::create_directories("/tmp/jpov_strip_degenerate");

    const std::string baseline = TmpPath("00_baseline");
    const std::string zero_area = TmpPath("10_zero_area");
    const std::string area = TmpPath("20_area");
    const std::string area_plus_normal = TmpPath("30_area_plus_normal");

    RenderTo(baseline, DegenerateApp::Mode::kNone);
    RenderTo(zero_area, DegenerateApp::Mode::kZeroArea);
    RenderTo(area, DegenerateApp::Mode::kArea);
    RenderTo(area_plus_normal, DegenerateApp::Mode::kAreaPlusNormal);
    LOG(INFO) << "[degenerate] 四张图已渲染（不崩即通过第 0 条断言）";

    // ① 零面积退化：一个像素都不许改。
    const DiffStats z = Compare(baseline, zero_area);
    CHECK(z.ok) << "对照图解码失败";
    LOG(INFO) << "[degenerate] zero-area: max=" << z.max_channel
              << " differing=" << z.differing;
    CHECK_LE(z.max_channel, 1)
        << "零面积的退化条带改变了画面（max diff " << z.max_channel
        << "）—— 说明退化几何真的产生了片元（零面积 quad 不该有片元）";

    // ② 有面积退化但**不污染**：条带区域之外必须逐像素相同。
    const DiffStats a = Compare(baseline, area);
    CHECK(a.ok) << "对照图解码失败";
    LOG(INFO) << "[degenerate] area-only: max=" << a.max_channel
              << " differing=" << a.differing << " outside=" << a.differing_outside
              << " outside_max=" << a.outside_max;
    CHECK_EQ(a.differing_outside, 0)
        << "条带区域之外出现差异（" << a.differing_outside
        << " px，max " << a.outside_max << "）—— NaN/Inf 从 WBO 泄漏到了整帧";

    // ③ 同批里的正常条带仍然可见（退化邻居没把它带走）。
    const DiffStats n = Compare(baseline, area_plus_normal);
    CHECK(n.ok) << "对照图解码失败";
    LOG(INFO) << "[degenerate] area+normal: max=" << n.max_channel
              << " differing=" << n.differing << " outside=" << n.differing_outside
              << " outside_max=" << n.outside_max;
    CHECK_EQ(n.differing_outside, 0)
        << "条带区域之外出现差异（" << n.differing_outside << " px，max " << n.outside_max
        << "）—— 退化条带把好条带/整帧一起污染了";
    CHECK_GT(n.differing, a.differing)
        << "同批的正常条带没有出现（differing=" << n.differing << " vs " << a.differing
        << "）—— 退化邻居可能让整批被丢弃";
    CHECK_GT(n.differing, 500) << "正常条带可见像素太少（" << n.differing << "）";

    LOG(INFO) << "strip degenerate probe passed";
    return 0;
}

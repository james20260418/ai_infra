// JPOV 条带 viewer「全家福」gold —— 元素级 no-op 护栏（2026-10-22 Danis 定）
//
// 为什么要这一份（与 strip_gold.cc 互补，不是重复）：
//   * strip_gold 锁的是 M1~M3 的**折线路径**，并且刻意关掉了圆环 / 射流 / fresnel
//     / bloom —— 它回答的是「条带的三条基础通路有没有坏」；
//   * 这份锁的是 **viewer 里所有元素同屏**的样子：三条带（纹理 / 纯色、柔边 / 硬边、
//     覆盖集 / 加法集、软深度）、玻璃圆环（fresnel + 两条 pass）、火箭射流（HDR +
//     逐点颜色 + 中心实两侧虚剖面）、bloom、近距离细节。
//
// 用途：重构（Strip16 收口 per-instance 属性：逐点 RGBA、逐条带宽度剖面、
// 去掉 batch tint / strip_tints / additive_gain）前后各拍一次并比较
// ⇒ 一次就能证明重构是 no-op（或精确指出哪些像素变了）。
//
// ⚠️ 比较不是「逐字节」，而是**逐像素 + 极小的 LSB 容差**（见 GoldTolerance）：
//   重构把纵向 alpha 并进了逐点颜色（改由纹理采样），而平滑 pass 的 u 是**重建后**
//   的 u*（颜色本来就这么采；现在 alpha 跟着一致），插值路径的浮点舍入不同 ⇒
//   平滑路径有个别像素差 1~2/255。这是**唯一**的语义变化（更一致，见 strip_types.h），
//   折线路径与圆环（60）仍然逐像素完全相同。
//
// 📌 2026-10-25 重出记录（三处改动，见设计文档 §19.12）：
//   ① **加法集早退门槛改成按注入量**（不再是覆盖率 α）—— 这**故意改画面**：HDR 颜色
//      （射流增益 3）下「α 极低但色极亮」的淡尾之前被丢掉，现在会出现。实测只在射流/弧
//      尾部多了 228~290 px、最大 8~9/255。⇒ 含射流的 6 张（10/20/30/40/50/70）重出。
//   ② 平滑 pass 的逐点颜色改从**控制点数据纹理**取（省一次过滤采样）：实测折线路径
//      （20）完全不受影响，平滑路径整片只有 ≤1 LSB 的差异（mix 与硬件双线性同解）；
//   ③ Schlick 的 power==5 走五次乘法：**60 号图逐字节相同**（旧 gold 未动，直接绿）。
//   证据：60 未重出、仍绿；80 未重出、仍绿（max=1，px>1=0）。
//   判据：只重出「因 ① 而必然变」的图，保留 60/80 旧图 —— 反过来证明改动没碰其它路径。
//
// 用法：
//   bazel run //tools/jpov/test/strip:strip_scene_gold_generator   # 重出 gold
//   bazel test //tools/jpov/test/strip:strip_scene_gold_test       # 回归
//
// 附录：三组**非恒真**断言（防止「画面整体没了 + 重出 gold」一起绿）：
//   ① 纹理路径：带纹理 vs 纯色的差异像素 ∈ [5000, 120000]；
//   ② 两条 pass：平滑 vs 折线的差异像素 ≥ 200（相等 ⇒ style 没接线）；
//   ③ 宽度剖面：柔边 vs 硬边的差异像素 ∈ [2000, 80000]。
// 这些统计都基于**本次实际渲染**，与 gold 无关，所以 gold 重出也不会让它们变恒真。

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

std::string AssetRoot() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    return srcdir != nullptr ? (std::string(srcdir) + "/__main__/")
                             : jpov::GetProjectRoot();
}

std::string StripAssetPath() {
    return AssetRoot() + "tools/jpov/assets/strip/ribbon_test.png";
}

// gold 文件名 = strip_scene_<shot>_<w>x<h>.png（与 shot.name 一一对应）。
std::string GoldPath(const char* shot_name) {
    return jpov::GetTestDataDir() + "/strip/strip_scene_" + shot_name + "_" +
           std::to_string(kWidth) + "x" + std::to_string(kHeight) + ".png";
}

// 单张 gold 的容差。默认 = 「逐像素相同或只差 1/255的个别像素」；
// 平滑 pass 因为 alpha 改为按重建后的 u* 采样（与颜色一致），允许 2/255 的少量像素。
struct GoldTolerance {
    int max_channel_diff = 1;   // 任一通道允许的最大差（0..255 域）
    long max_pixels_over_1 = 0; // 差 > 1 的像素数上限
    long max_pixels_any = 64;   // 有任何差异的像素数上限（捕捉「整片偏移」）
};
inline constexpr GoldTolerance kLsb1{1, 0, 64};
inline constexpr GoldTolerance kSmoothLsb2{2, 200, 8000};

// 一张图的完整开关。默认值 = viewer 交互模式的初始状态（与 strip_viewer_app.h 对齐）；
// 每张都**完整**覆盖所有开关 —— 漏还原一个会静默污染后面的图（上一版拍摄表的教训）。
struct Shot {
    const char* name;
    bool ribbons = true;         // 三条带（波 / 螺旋 / 弧）
    bool ring = true;            // 玻璃圆环（四组点 → 四条带）
    bool jet = true;             // 火箭射流（加法集 + HDR + 逐点颜色）
    bool use_texture = true;     // false = 纯色（不看纹理）
    bool soft_edges = true;      // 宽度剖面：柔边 / 硬边
    bool arc_additive = false;   // 弧形改走加法集（发光拖尾）
    bool soft_depth = false;     // 条带与几何相交处的软过渡
    bool smooth = true;          // 三条带 / 射流：平滑 vs 折线
    bool ring_smooth = true;     // 圆环单独一个 pass 开关（与拍摄表同样的历史约定）
    bool fresnel = true;         // 菲涅耳边反（只作用于圆环）
    int fresnel_preset = 0;      // kFresnelPresets 下标
    bool ring_at_origin = false; // 圆环挪到原点（只看圆环时居中）
    bool boxes = true;           // 背景参考方块
    bool bloom = true;           // 辉光（射流 HDR 高光）
    float jet_gain = 3.0f;       // 射流 HDR 增益
    float camera_r = jpov_strip::kViewDefaults.R;
    GoldTolerance tolerance = kLsb1;
};

// 拍摄表。改名 = 改 gold 文件名，必须同时重出（generator 会写新文件、旧文件不会自动删）。
const Shot kShots[] = {
    // ① 全家福（平滑）：三条带（纹理 + 柔边）+ 圆环（fresnel）+ 射流（bloom）。
    {.name = "10_scene_full_smooth", .tolerance = kSmoothLsb2},
    // ② 同一构图换成折线 + 折面法线：锁住另一条 pass。
    {.name = "20_scene_full_polyline", .smooth = false, .ring_smooth = false},
    // ③ 三条带改纯色（不采样纹理）：纯色路径 + 逐条带浓度（旧 strip_tints 的等价物）。
    {.name = "30_scene_solid", .use_texture = false, .tolerance = kSmoothLsb2},
    // ④ 硬边剖面：逐条带 width_profile 的另一半取值。
    {.name = "40_scene_hard_edges", .soft_edges = false, .tolerance = kSmoothLsb2},
    // ⑤ 弧形走加法集 + 软深度：加法集与软深度在完整场景里的样子。
    {.name = "50_scene_arc_additive_soft_depth", .arc_additive = true,
     .soft_depth = true, .tolerance = kSmoothLsb2},
    // ⑥ 圆环单独 + 厚玻璃预设（近距离）：fresnel 与平滑重建的主场景。
    {.name = "60_ring_fresnel_thick", .ribbons = false, .jet = false,
     .fresnel_preset = 2, .ring_at_origin = true, .boxes = false,
     .camera_r = 3.6f},
    // ⑦ 射流单独：加法集 HDR（逐点颜色 × 增益）+ 中心实两侧虚剖面。
    {.name = "70_jet_hdr", .ribbons = false, .ring = false},
    // ⑧ 拉近的全家福：亚像素细节 —— 逐点颜色 / alpha / 剖面换成纹理采样后，
    //    最容易在这里露出 1 LSB 以上的差异。
    {.name = "80_scene_zoom", .camera_r = 2.6f, .tolerance = kSmoothLsb2},
};

// 按 shot 配置拍一张。
void RenderTo(const std::string& out_path, const Shot& shot) {
    JPOV::Config cfg;
    cfg.title = "JPOV strip scene gold";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    jpov_strip::StripViewerApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.Setup(StripAssetPath());
    jpov_strip::ApplyDefaultView(&app);

    app.show_strips_ = true;
    app.vis_ = jpov_strip::RibbonVisibility{shot.ribbons, shot.ribbons, shot.ribbons};
    app.ring_visible_ = shot.ring;
    app.jet_visible_ = shot.jet;
    app.jet_gain_ = shot.jet_gain;
    app.bloom_ = shot.bloom;
    app.boxes_visible_ = shot.boxes;
    app.use_texture_ = shot.use_texture;
    app.soft_edges_ = shot.soft_edges;
    app.arc_additive_ = shot.arc_additive;
    app.soft_depth_ = shot.soft_depth;
    app.smooth_strips_ = shot.smooth;
    app.ring_smooth_ = shot.ring_smooth;
    app.fresnel_enabled_ = shot.fresnel;
    app.fresnel_preset_ = shot.fresnel_preset;
    app.fresnel_ = jpov_strip::kFresnelPresets[shot.fresnel_preset].params;
    app.ring_shape_ = jpov_strip::MakeSceneRingShape();
    if (shot.ring_at_origin) {
        app.ring_shape_.center_x = 0.0f;
        app.ring_shape_.center_z = 0.0f;
        app.ring_shape_.center_y = 1.75f;   // 抬到参考方块顶（0.995）之上
    }
    app.view_.R = shot.camera_r;

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

// 两张图的差异统计（0..255 域，逐像素取三通道最大差）。
struct DiffStats {
    int max_channel = 0;   // 最大单通道差
    long pixels_over_1 = 0;
    long pixels_any = 0;
    bool loaded = false;   // 解码/尺寸失败 → false
};

DiffStats CompareImages(const std::string& a_path, const std::string& b_path) {
    DiffStats stats;
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
        return stats;
    }
    for (int i = 0; i < aw * ah; ++i) {
        const int d = std::max(std::abs(int(a[i * 3 + 0]) - int(b[i * 3 + 0])),
                               std::max(std::abs(int(a[i * 3 + 1]) - int(b[i * 3 + 1])),
                                        std::abs(int(a[i * 3 + 2]) - int(b[i * 3 + 2]))));
        if (d > 0) {
            ++stats.pixels_any;
            stats.max_channel = std::max(stats.max_channel, d);
            if (d > 1) {
                ++stats.pixels_over_1;
            }
        }
    }
    stbi_image_free(a);
    stbi_image_free(b);
    stats.loaded = true;
    return stats;
}

// 比较 actual 与 gold：按 shot 的容差检查，失败时给出完整统计再 FATAL
// （便于判断是「完全没渲染」「整片偏移」还是「个别 LSB」）。
void CheckAgainstGold(const std::string& actual_path, const Shot& shot) {
    const std::string gold_path = GoldPath(shot.name);
    const DiffStats d = CompareImages(actual_path, gold_path);
    if (!d.loaded) {
        LOG(FATAL) << "gold 缺失或尺寸不一致: " << gold_path
                   << "（先跑 strip_scene_gold_generator）";
    }
    const GoldTolerance& tol = shot.tolerance;
    if (d.max_channel > tol.max_channel_diff || d.pixels_over_1 > tol.max_pixels_over_1 ||
        d.pixels_any > tol.max_pixels_any) {
        LOG(FATAL) << "gold 不一致: " << shot.name << "（实际=" << actual_path
                   << "，gold=" << gold_path << "）max_channel=" << d.max_channel
                   << "（允许 " << tol.max_channel_diff << "）px>1=" << d.pixels_over_1
                   << "（允许 " << tol.max_pixels_over_1 << "）px>0=" << d.pixels_any
                   << "（允许 " << tol.max_pixels_any << "）";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool generate = (argc > 1 && std::strcmp(argv[1], "--generate") == 0);
    const std::string tmp_dir = "/tmp/jpov_strip_scene_gold";
    std::filesystem::create_directories(tmp_dir);

    // actual 一律先落到临时目录（generator 时再拷到 gold 路径）。
    constexpr size_t kShotCount = sizeof(kShots) / sizeof(kShots[0]);
    std::vector<std::string> actual_paths;
    actual_paths.reserve(kShotCount);
    for (size_t i = 0; i < kShotCount; ++i) {
        const std::string path =
            tmp_dir + "/" + std::to_string(i) + "_" + kShots[i].name + ".png";
        RenderTo(path, kShots[i]);
        actual_paths.push_back(path);
        LOG(INFO) << "rendered: " << kShots[i].name;
    }

    if (generate) {
        for (size_t i = 0; i < actual_paths.size(); ++i) {
            const std::string gold = GoldPath(kShots[i].name);
            std::filesystem::create_directories(
                std::filesystem::path(gold).parent_path());
            std::filesystem::copy_file(actual_paths[i], gold,
                                       std::filesystem::copy_options::overwrite_existing);
            LOG(INFO) << "gold written: " << gold;
        }
        return 0;
    }

    for (size_t i = 0; i < actual_paths.size(); ++i) {
        CheckAgainstGold(actual_paths[i], kShots[i]);
        LOG(INFO) << "[gold] ok: " << kShots[i].name;
    }

    // ① 纹理路径真的生效（带纹理 vs 纯色）；② 两条 pass 真的不同；③ 柔边/硬边真的不同。
    // 下标直接按拍摄表取，改名会编译期报错（kShots 是 constexpr 数组）。
    const long textured_diff = CompareImages(actual_paths[0], actual_paths[2]).pixels_any;
    CHECK_GE(textured_diff, 5000) << "带纹理与纯色几乎一致 → 纹理可能没绑上";
    CHECK_LE(textured_diff, 120000) << "带纹理与纯色差异过大 → 画面可能整体被改";
    LOG(INFO) << "[assert] textured-vs-solid diff px = " << textured_diff;

    const long style_diff = CompareImages(actual_paths[0], actual_paths[1]).pixels_any;
    CHECK_GE(style_diff, 200) << "平滑与折线几乎一致 → style 可能没接线";
    LOG(INFO) << "[assert] smooth-vs-polyline diff px = " << style_diff;

    const long profile_diff = CompareImages(actual_paths[0], actual_paths[3]).pixels_any;
    CHECK_GE(profile_diff, 2000) << "柔边/硬边几乎一样 → width_profile 可能没生效";
    CHECK_LE(profile_diff, 80000) << "柔边/硬边差异过大 → 剖面可能溢出";
    LOG(INFO) << "[assert] soft-vs-hard diff px = " << profile_diff;

    LOG(INFO) << "strip scene gold test passed";
    return 0;
}

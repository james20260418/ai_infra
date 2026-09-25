// JPOV FBX 观察器 gold test —— 渲染门禁
//
// 与 generator 共用 jpov_fbx_pose_gold_common.h（= 直接跑观察器本体），渲三帧到 temp：
//   ① kGoldTimeSeconds（第 60 帧）→ 与仓库 gold 做 ROI 平均色对比（同其它 gold 门禁）；
//   ② kOtherTimeSeconds（第 150 帧）→ 必须与 ① 不同（证明"时间→位姿→几何"确实在更新，
//      否则两帧会渲成同一张图）；
//   ③ rest 模式（identity 位姿）→ 必须与 ① 不同（证明 rest 复选框真的换了位姿来源）。
//
// 另外几道门禁：
//   ④ 传 glb → 「两者并列」渲一帧 → 与第二张 gold 比对；并统计**强蓝像素**：
//      无蓝物体的基线图必须 ~0、含蓝骨人的图必须明显 >0（蓝骨真渲出来了）。
//   ⑤ 正式重定向路径（kBothRetarget）→ 根位移量级 + 蓝骨可见性 + 第三张 gold。
//   ⑥ **组合拳**（本次新增）：fbx 动作（BodyRetarget）驱动 glb **带皮**动画 +
//      **3 个实例**（一个 SkinnedMeshCommand / 一次 instanced draw）+ **部位粗细 μ**。
//      一张 gold 盖住蓝侧这条链路的三个环，邻路再带两道**逐像素**非恒真门禁：
//      μ=(1.6,0.6) vs μ≡1（证明系数真的到了 shader）、t=2.0s vs 5.0s（证明重定向动画在驱动）。
//      为何不只用 gold：8×8 ROI 均值对只占全图 ~0.6% 的小人会稀释到看不出（见 PerPixelDiffStats）。
//      ⚠️ 已知边界：gold 无法判「3 个实例是不是**一次** draw 画完」（需 apitrace/GL_PROXY）。
// 可见性门禁（基础）：场景里唯一的红色物体就是火柴人，故全图"强红"像素数若塌向 0，
// 说明火柴人没渲出来（headless 出图不画面板与文字，没有任何其它红色来源）。
// ⚠️ 蓝色不能用同款"全图计数"判：天空本身就是蓝的 —— 故用**严格蓝**判据
//    (b>150 && r<120 && g<140)，实测天空最蓝像素 r=148 被排掉、蓝骨像素 ≈(21,20,199)。
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

std::string GlbGoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/tools/jpov/test" +
               jpov_fbx_pose_gold::GetGlbNaiveGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_fbx_pose_gold::GetGlbNaiveGoldRelPath();
}

std::string RetargetGoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/tools/jpov/test" +
               jpov_fbx_pose_gold::GetGlbRetargetGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_fbx_pose_gold::GetGlbRetargetGoldRelPath();
}

std::string ThicknessGoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/tools/jpov/test" +
               jpov_fbx_pose_gold::GetSkinnedThicknessGoldRelPath();
    }
    return jpov::GetProjectRoot() + "tools/jpov/test" +
           jpov_fbx_pose_gold::GetSkinnedThicknessGoldRelPath();
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

// 整图"严格蓝"像素数（火柴人蓝骨人；判据需排除天空 —— 见文件头注释）。
long long CountStrongBluePixels(const std::string& png) {
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
        if (b > 150 && r < 120 && g < 140) {
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
    const std::string out_glb_both = outdir + "glb_both.png";
    const std::string out_glb_retarget = outdir + "glb_retarget.png";
    const std::string out_skinned_thick = outdir + "skinned_instanced_thick.png";
    const std::string out_skinned_neutral = outdir + "skinned_instanced_neutral.png";
    const std::string out_skinned_other = outdir + "skinned_instanced_other_time.png";

    // ── 四帧都由观察器本体渲出（走共用的 MakeApp，与 generator 零分叉）──
    //   ⚠️ 分两个 App："无 glb"与"有 glb"是**两种机位**（LoadGltf 会按并列重算初始机位），
    //   基础 gold 必须与 generator 的①同构（无 glb），否则会被相机差异误报为回归。
    {
        std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app =
            jpov_fbx_pose_gold::MakeApp("JPOV FBX Pose Gold Test",
                                        jpov_fbx_pose_gold::kGoldTimeSeconds,
                                        /*rest_pose*/ false, /*glb_path*/ "",
                                        jpov_fbx_viewer::ViewMode::kFbxOnly);
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_gold_frame.c_str());
        app->anim_time_seconds_ = jpov_fbx_pose_gold::kOtherTimeSeconds;
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_other_frame.c_str());
        app->rest_pose_mode_ = true;
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_rest_pose.c_str());
    }
    {
        // ④ 对照组：含 glb（两者并列）同帧→ 蓝骨人（glb rest 被同一份 pose **数值直搬**）。
        //    ⚠️ 必须与 generator 的②同模式（kBothNoRetarget）——它比对的就是这张 gold。
        std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app =
            jpov_fbx_pose_gold::MakeApp("JPOV FBX Pose Gold Test",
                                        jpov_fbx_pose_gold::kGoldTimeSeconds,
                                        /*rest_pose*/ false,
                                        jpov_fbx_pose_gold::GlbPath(),
                                        jpov_fbx_viewer::ViewMode::kBothNoRetarget);
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_glb_both.c_str());
    }

    // ⑤ 正式**重定向**路径（kBothRetarget）—— 曾经的覆盖盲区（见下）。
    //    🔴 历史 bug（2026-09-20）：重定向把源帧的 root_offset（**源单位 cm**）当成米用，
    //    位移被静默放大 100 倍（实测最大 63.6 m）→ 蓝骨人/蓝带皮飞出画面。
    //    旧 gold 只盖了 naive 对照路径（它把 root_offset 置 0），故静默通过。本节补上三条门禁：
    //      (a) 数值：重定向后根位移必须与人身高相称（< 2 m）；
    //      (b) 可见性：蓝骨人必须还在画面里（飞走则严格蓝像素骤降）；
    //      (c) gold 字节级对比：锁住该路径的固定帧画面。
    {
        std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app =
            jpov_fbx_pose_gold::MakeApp("JPOV FBX Pose Gold Test",
                                        jpov_fbx_pose_gold::kGoldTimeSeconds,
                                        /*rest_pose*/ false,
                                        jpov_fbx_pose_gold::GlbPath(),
                                        jpov_fbx_viewer::ViewMode::kBothRetarget);
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_glb_retarget.c_str());

        // (a) 数值门禁：根位移量级。
        const jpov::Vec3f& ro = app->glb_pose_for_test().root_offset;
        LOG(INFO) << "重定向根位移（本帧）: |root_offset| = " << ro.Norm() << " m";
        CHECK_LT(ro.Norm(), 2.0f)
            << "🔴 重定向后的根位移过大（" << ro.Norm() << " m）—— 蓝骨人会飞出画面。"
               "多为「骨架与位姿不同单位」（忘记把源长量归一为米）。";
        // 且确实非零（root-motion 真接上了，而不是又被丢掉）。
        CHECK_GT(ro.Norm(), 1e-4f)
            << "重定向后根位移几乎为 0 —— root-motion 没接上？";

        // (b) 可见性门禁：蓝骨人仍在画面内。
        const long long blue_retarget = CountStrongBluePixels(out_glb_retarget);
        LOG(INFO) << "蓝骨可见性(重定向路径): 严格蓝像素 = " << blue_retarget;
        CHECK_GT(blue_retarget, 15)
            << "🔴 重定向路径下蓝骨人未渲出/已飞走（严格蓝像素仅 " << blue_retarget << "）";

        // (c) gold 对比。
        const std::string rt_gold = RetargetGoldPath();
        {
            FILE* f = std::fopen(rt_gold.c_str(), "rb");
            CHECK(f != nullptr) << "重定向 gold 缺失，请先跑 generator: " << rt_gold;
            std::fclose(f);
        }
        constexpr double kRtGoldThreshold = 25.0;
        const double rt_diff =
            jpov::CompareLightMeanRoiPng(rt_gold, out_glb_retarget, 8, 8);
        LOG(INFO) << "GLB RETARGET GOLD COMPARE: max-channel-mean-diff = " << rt_diff
                  << " (threshold=" << kRtGoldThreshold << ")";
        if (rt_diff < 0 || rt_diff > kRtGoldThreshold) {
            LOG(ERROR) << "GLB RETARGET GOLD COMPARE FAILED: " << rt_diff;
            return 1;
        }
    }

    // ⑥ 组合拳（本次新增）：**3 实例 instanced 带皮 + BodyRetarget 驱动 + 部位粗细 μ**。
    //    一张 gold 同时钉住蓝侧这条链路的三个环（instance 绘制 / 重定向驱动 / thickness），
    //    任一环断掉本图必变；邻路再加两道**逐像素**门禁（gold 的 8×8 ROI 均值对只占全图
    //    ~0.6% 的小人会稀释到看不出来，故不能只靠 gold）：
    //      (a) μ=(1.6,0.6) vs μ=(1,1) 同帧对比 → 差异只能来自 μ（证明 per-instance 系数
    //          真的送到 shader 了，且带皮实例真的存在）；
    //      (b) 时刻 2.0s vs 5.0s 同 μ 对比 → 差异只能来自位姿（证明重定向动画真的在驱动实例）。
    {
        std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app =
            jpov_fbx_pose_gold::MakeSkinnedThicknessApp(
                "JPOV FBX Skinned Instanced Thickness Gold Test",
                jpov_fbx_pose_gold::kGoldTimeSeconds,
                jpov_fbx_pose_gold::kThickLegGold, jpov_fbx_pose_gold::kThickArmGold);
        // 三帧都在**设定时刻**出图（paused_ 只停时间自行推进，不影响本帧渲染）：
        //   否则第二帧会叠加动画自身的变化，"μ 是否生效"就分不清了。
        app->paused_ = true;
        app->anim_time_seconds_ = jpov_fbx_pose_gold::kGoldTimeSeconds;
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_skinned_thick.c_str());
        // 对照帧 1：同模式 / 同帧 / 同相机，只把 μ 归 1（= 不做粗细）。
        app->thickness_leg_ = 1.0f;
        app->thickness_arm_ = 1.0f;
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_skinned_neutral.c_str());
        // 对照帧 2：同模式 / 同 μ，只换时刻（位姿不同）。
        app->thickness_leg_ = jpov_fbx_pose_gold::kThickLegGold;
        app->thickness_arm_ = jpov_fbx_pose_gold::kThickArmGold;
        app->anim_time_seconds_ = jpov_fbx_pose_gold::kOtherTimeSeconds;
        jpov_fbx_pose_gold::RenderFrame(app.get(), out_skinned_other.c_str());
    }

    // (a) μ 真的送到 GPU 了：与 μ≡1 的对照帧必须有可见差异。
    //     噪声底 ≈ 0：同帧同姿态的两次渲染走同一条 OneIteration，既有 gold 门禁实测逐字节相同
    //     ⇒ 出现差异只能是 μ 改了几何。门禁取 300 像素（远高于噪声底；实测见 log）。
    {
        int max_abs = 0;
        long long moved_px = 0;
        PerPixelDiffStats(out_skinned_neutral, out_skinned_thick, &max_abs, &moved_px);
        LOG(INFO) << "μ 生效门禁: μ=(1.6,0.6) vs μ=(1,1) 逐像素 max-abs=" << max_abs
                  << " 差>30 像素数=" << moved_px;
        CHECK_GT(max_abs, 30) << "改 μ 后画面几乎没变（per-instance 系数没送到 shader？）";
        CHECK_GT(moved_px, 300) << "改 μ 后变化像素过少: " << moved_px;
    }

    // (b) 重定向动画真的在驱动这 3 个实例：换时刻必须换画面（同一 μ、同一相机）。
    {
        int max_abs = 0;
        long long moved_px = 0;
        PerPixelDiffStats(out_skinned_thick, out_skinned_other, &max_abs, &moved_px);
        LOG(INFO) << "组合拳动态门禁: t=2.0s vs 5.0s 逐像素 max-abs=" << max_abs
                  << " 差>30 像素数=" << moved_px;
        CHECK_GT(max_abs, 60) << "不同时刻的带皮实例几乎一样（重定向位姿没到 atlas/没换帧？）";
        CHECK_GT(moved_px, 100) << "不同时刻变化像素过少: " << moved_px;
    }

    // (c) gold 比对（组合拳固定帧）。
    {
        const std::string th_gold = ThicknessGoldPath();
        {
            FILE* f = std::fopen(th_gold.c_str(), "rb");
            CHECK(f != nullptr) << "组合拳 gold 缺失，请先跑 generator: " << th_gold;
            std::fclose(f);
        }
        constexpr double kThGoldThreshold = 25.0;
        const double th_diff =
            jpov::CompareLightMeanRoiPng(th_gold, out_skinned_thick, 8, 8);
        LOG(INFO) << "SKINNED INSTANCED THICKNESS GOLD COMPARE: max-channel-mean-diff = "
                  << th_diff << " (threshold=" << kThGoldThreshold << ")";
        if (th_diff < 0 || th_diff > kThGoldThreshold) {
            LOG(ERROR) << "SKINNED INSTANCED THICKNESS GOLD COMPARE FAILED: " << th_diff;
            return 1;
        }
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

    // ── glb 对照组第四帧：gold 比对 + 蓝骨可见性门禁 ──
    {
        const std::string glb_gold_path = GlbGoldPath();
        {
            FILE* f = std::fopen(glb_gold_path.c_str(), "rb");
            CHECK(f != nullptr) << "glb 对照组 gold 缺失，请先跑 generator: "
                                << glb_gold_path;
            std::fclose(f);
        }
        // 蓝骨可见性：无蓝物体的基线图应 ~0，含蓝骨人的图应明显 >0。
        const long long blue_baseline = CountStrongBluePixels(out_gold_frame);
        const long long blue_both = CountStrongBluePixels(out_glb_both);
        LOG(INFO) << "蓝骨可见性: 基线(仅红)= " << blue_baseline
                  << "，两者并列= " << blue_both;
        CHECK_LT(blue_baseline, 10)
            << "基线图（没传蓝骨人）不应有严格蓝像素，实测 " << blue_baseline;
        CHECK_GT(blue_both, 15)
            << "🔴 蓝骨人未渲出（严格蓝像素仅 " << blue_both << "）";

        constexpr double kGlbGoldThreshold = 25.0;
        const double glb_diff =
            jpov::CompareLightMeanRoiPng(glb_gold_path, out_glb_both, 8, 8);
        LOG(INFO) << "GLB GOLD COMPARE: max-channel-mean-diff = " << glb_diff
                  << " (threshold=" << kGlbGoldThreshold << ")";
        if (glb_diff < 0 || glb_diff > kGlbGoldThreshold) {
            LOG(ERROR) << "GLB GOLD COMPARE FAILED: " << glb_diff;
            return 1;
        }
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

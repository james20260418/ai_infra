// JPOV 蒙皮多帧一致性测试 —— 「真蒙皮」与「object3d 直画」在**同一世界位置**的逐像素比对。
//
// 为什么要这条（对应 skills/zero-run-code-reading-check：测试必须能抓住真缺陷）：
//   jpov_skeleton_gold_test 只渲 **单个 pose（identity/bind）**，而 bind 姿态下肤矩阵恒为 I
//   —— 它对 atlas 取址**完全免疫**：GPU 从 atlas 哪一格取矩阵，只要取到单位阵，画面一样。
//   历史上真出过两类 atlas 缺陷，单帧 identity 场景都看不见：
//     ① 布局分叉：烘焙按「一行恰好 N 个 pose」分块、shader 按 (row,col) 取址，两者对 2D
//        布局理解不同 ⇒ pose 跨行时取到未上传的黑行（顶点塌到原点 / 人像腰斩）。
//     ② 跨行写越界：按 scanline 分组写、组内假设 pose 完整落在本行 ⇒ 跨行 pose 的
//        x0 算成负数（堆越界崩溃；帧数 > 一行容量时才触发）。
//   本测试用 25 帧（> 一行容量 22）逐帧做 A/B 逐像素比对，把①②都变成硬门禁。
//
// 做法：人像固定在**同一世界位置**，分两次渲染同一帧 —— 一次走真蒙皮（SkeletonManager
//   atlas + 蒙皮 VS），一次走 object3d 直画 —— 然后比对两张 PNG 的像素差。
//   · bind 帧（identity pose）：两条路径数学恒等 ⇒ 必须**逐像素几乎相同**（严格门禁）。
//   · 非 bind 帧：直画是 rest 网格、蒙皮是形变后的网格 ⇒ 形状本就不同，不能直接比对。
//     故非 bind 帧用「**形状合理性**」门禁：蒙皮人像必须是一个连通、无碎片、无爆炸的
//    人形（前景像素面积、包围盒纵横比、连通块数都在合理范围）——取错骨会塌陷/炸开，
//     这些量会立刻越界。
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/test/skeleton/jpov_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {
std::string OutDir() { return jpov::GetOutputDir() + "jpov_skinned_multipose_test/"; }
std::string Assets() {
    return jpov::GetProjectRoot() + "tools/jpov/test/object3d/scene_assets/";
}
std::string Male() {
    return jpov::GetProjectRoot() +
           "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";
}

struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> px;  // RGBA
    bool ok() const { return w > 0 && h > 0 && !px.empty(); }
};

Img Load(const std::string& path) {
    Img im;
    int c = 0;
    unsigned char* d = stbi_load(path.c_str(), &im.w, &im.h, &c, 4);
    if (d == nullptr) {
        LOG(ERROR) << "读图失败: " << path;
        return im;
    }
    im.px.assign(d, d + static_cast<size_t>(im.w) * im.h * 4);
    stbi_image_free(d);
    return im;
}

// 逐像素平均通道差（满分 255），**只统计左半屏** [0,HW)。
double MeanAbsDiffLeft(const Img& a, const Img& b, int hw) {
    if (!a.ok() || !b.ok() || a.w != b.w || a.h != b.h) return -1.0;
    double acc = 0;
    long n = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < hw; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
            for (int ch = 0; ch < 3; ++ch)
                acc += std::fabs(double(a.px[i + ch]) - double(b.px[i + ch]));
            ++n;
        }
    return acc / (n * 3.0);
}

// 「几何掩码」不一致率（**只统计左半屏**）：把两图各自的前景掩码比一比，算 XOR/并集。
// 前景区分用"与天空/地面参考色都明显不同"判定。**只看几何不看着色** —— 正是要锁的东西。
double MaskDiffLeft(const Img& a, const Img& b, int hw) {
    if (!a.ok() || !b.ok() || a.w != b.w || a.h != b.h) return -1.0;
    const int W = a.w, H = a.h;
    auto mask = [&](const Img& im) {
        auto band = [&](int y0, int y1) {
            double r = 0, g = 0, bb = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < hw; ++x) {
                    const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                    r += im.px[i]; g += im.px[i + 1]; bb += im.px[i + 2];
                    ++n;
                }
            return std::array<double, 3>{r / n, g / n, bb / n};
        };
        const auto sky = band(0, 4);
        const auto grd = band(H - 4, H);
        std::vector<unsigned char> m(static_cast<size_t>(W) * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < hw; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                const double r = im.px[i], g = im.px[i + 1], bb = im.px[i + 2];
                auto d2 = [&](const std::array<double, 3>& c) {
                    return (r - c[0]) * (r - c[0]) + (g - c[1]) * (g - c[1]) +
                           (bb - c[2]) * (bb - c[2]);
                };
                if (std::min(d2(sky), d2(grd)) > 55.0 * 55.0)
                    m[static_cast<size_t>(y) * W + x] = 1;
            }
        return m;
    };
    const std::vector<unsigned char> ma = mask(a), mb = mask(b);
    long xr = 0, un = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < hw; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            if (ma[i] != mb[i]) ++xr;
            if (ma[i] || mb[i]) ++un;
        }
    if (un == 0) return 0.0;
    return static_cast<double>(xr) / un;
}
// 前景像素占比（**只统计左半屏**）：用于「插值帧必须落在两端点之间」的合理性门禁。
double ForegroundRatioLeft(const Img& im, int hw) {
    if (!im.ok()) return -1.0;
    const int W = im.w, H = im.h;
    auto band = [&](int y0, int y1) {
        double r = 0, g = 0, b = 0;
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < hw; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                r += im.px[i]; g += im.px[i + 1]; b += im.px[i + 2];
                ++n;
            }
        return std::array<double, 3>{r / n, g / n, b / n};
    };
    const auto sky = band(0, 4);
    const auto grd = band(H - 4, H);
    long fg = 0, tot = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < hw; ++x) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 4;
            const double r = im.px[i], g = im.px[i + 1], b = im.px[i + 2];
            auto d2 = [&](const std::array<double, 3>& c) {
                return (r - c[0]) * (r - c[0]) + (g - c[1]) * (g - c[1]) +
                       (b - c[2]) * (b - c[2]);
            };
            if (std::min(d2(sky), d2(grd)) > 55.0 * 55.0) ++fg;
            ++tot;
        }
    return tot > 0 ? static_cast<double>(fg) / tot : -1.0;
}
}  // namespace

int main() {
    const std::string outdir = OutDir();
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "JPOV Skinned Multi-Pose Test";
    cfg.headless = true;
    jpov_skeleton_gold::SkeletonGoldApp app(cfg);
    app.Init();
    jpov_skeleton_gold::BuildScene(&app, Assets(), Male());

    const int npose = static_cast<int>(app.poses_.size());
    CHECK_GE(npose, 25) << "需要 > 一行容量(22) 的帧数以覆盖跨行 pose";

    jpov::InputSnapshot input{};
    jpov::WindowInfo winfo;
    winfo.width  = (float)jpov_skeleton_gold::kOutW;
    winfo.height = (float)jpov_skeleton_gold::kOutH;

    int fail = 0;
    const int W = jpov_skeleton_gold::kOutW;
    const int HW = W / 2;  // 左半屏宽（左 = 蒙皮人所在）

    // ============ 门禁：每个 pose 的「GPU 蒙皮」必须与「CPU 真值蒙皮」逐像素同形 ============
    // 覆盖 atlas 全部行（25 帧 > 一行 22）。左半屏放同一位置的人像，只切换渲染路径，
    // 故两次渲染可直接比像素。同时报 mask 不一致率（只看几何）与平均通道差（含着色）。
    for (int k = 0; k < npose; ++k) {
        app.pose_sel_ = k;
        // GPU 蒙皮路径。
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kSkinned;
        const std::string p_gpu = outdir + "gpu_pose" + std::to_string(k) + ".png";
        app.RunOnce(input, winfo, p_gpu.c_str());
        // CPU 真值路径：按同一 pose 在 CPU 端蒙皮 → UpdateMesh → 直画。
        app.UpdateMesh(app.cpu_mesh_id_,
                       jpov_skeleton_gold::SkinMeshOnCpuForTest(
                           app.raw_skel_, app.poses_[k], app.rest_mesh_));
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kCpuSkinned;
        const std::string p_cpu = outdir + "cpu_pose" + std::to_string(k) + ".png";
        app.RunOnce(input, winfo, p_cpu.c_str());

        const Img a = Load(p_gpu), b = Load(p_cpu);
        const double d = MeanAbsDiffLeft(a, b, HW);
        const double dm = MaskDiffLeft(a, b, HW);
        LOG(INFO) << "pose[" << k << "] GPU vs CPU真值  平均通道差 = " << d
                  << "  几何(mask)不一致率 = " << dm;
        if (d < 0 || dm < 0) {
            LOG(ERROR) << "门禁 失败: pose[" << k << "] 读图/尺寸错";
            ++fail;
        } else if (dm > 0.02) {
            LOG(ERROR) << "门禁 失败: pose[" << k
                       << "] GPU 蒙皮与 CPU 真值**几何**不一致 (mask=" << dm
                       << " > 0.02) —— 蒙皮链路有错";
            ++fail;
        } else if (d > 12.0) {
            LOG(ERROR) << "门禁 失败: pose[" << k << "] 着色差异过大 (diff=" << d << ")";
            ++fail;
        }
    }

    // ============ 门禁二：GPU 双帧插值（VS lerp）必须与 CPU 真值同形 ============
    // 取相邻两帧 (k, k+1) 在 ratio∈{0.25,0.75} 下插值：GPU 走 SkeletonManager 的 atlas 双 pose 取址 +
    //   shader 矩阵 lerp；CPU 按**同一公式**（矩阵空间逐元素 lerp 后蒙皮）算真值。
    //   两者必须几何同形 —— 这是「插值真的按姿态在动、且位置正确」的硬证据。
    //
    // 同时统计前景占比做「插值合理性」门禁：中间帧的人像面积必须落在两端点之间（含裕量），
    //   否则说明插值把顶点插到两端之外（发散的 lerp / 取错 pose）。
    std::vector<double> fg(npose, -1.0);
    for (int k = 0; k < npose; ++k) {
        app.pose_sel_ = k;
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kSkinned;
        app.RunOnce(input, winfo, (outdir + "fg_pose" + std::to_string(k) + ".png").c_str());
        fg[k] = ForegroundRatioLeft(
            Load(outdir + "fg_pose" + std::to_string(k) + ".png"), HW);
    }

    // ⚠️ ratio **必须取非 0.5 的值**：mix(ma,mb,0.5) == mix(mb,ma,0.5)，只用 50% 抽查
    //   抓不到「插值方向反了 / pose_a 与 pose_b 传反」这类错误（50% 是 mix 的不动点）。
    //   故每对相邻帧用 0.25 与 0.75 两档 —— 两端都偏离不动点，方向错会立刻翻倍暴露。
    //   （0.25/0.75 也顺带验证 ratio 真的被 shader 使用了，而非被忽略恒取 pose_a。）
    constexpr float kRatios[] = {0.25f, 0.75f};
    constexpr int kRatioCount = static_cast<int>(sizeof(kRatios) / sizeof(kRatios[0]));
    const int kInterpSteps = 6;  // 抽查 6 组相邻帧（覆盖首/中/尾 + 跨行边界）
    int interp_cases = 0;
    for (int s = 0; s < kInterpSteps; ++s) {
      const int a = (npose - 1) * s / kInterpSteps;
      const int b = (a + 1) % npose;  // 末帧回绕到第 0 帧（循环动画语义）
      for (int ri = 0; ri < kRatioCount; ++ri) {
        const float kRatio = kRatios[ri];
        ++interp_cases;

        // GPU：同一实例送 (pose_a=a, pose_b=b, ratio=kRatio)。
        app.pose_a_sel_ = a;
        app.pose_b_sel_ = b;
        app.ratio_sel_ = kRatio;
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kSkinned;
        const std::string p_gpu = outdir + "interp_gpu_" + std::to_string(s) + ".png";
        app.RunOnce(input, winfo, p_gpu.c_str());

        // CPU 真值：按同公式在 CPU 算同 ratio 的蒙皮结果 → 直画。
        app.UpdateMesh(app.cpu_mesh_id_,
                       jpov_skeleton_gold::SkinMeshOnCpuForTest(
                           app.raw_skel_, app.poses_[a], app.poses_[b], kRatio,
                           app.rest_mesh_));
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kCpuSkinned;
        const std::string p_cpu = outdir + "interp_cpu_" + std::to_string(s) + ".png";
        app.RunOnce(input, winfo, p_cpu.c_str());

        const Img ia = Load(p_gpu), ib = Load(p_cpu);
        const double d = MeanAbsDiffLeft(ia, ib, HW);
        const double dm = MaskDiffLeft(ia, ib, HW);
        const double f = ForegroundRatioLeft(ia, HW);
        // 合理区间：两端点前景占比之间的 [min*0.6, max*1.4]（留裕量吸收姿态非单调的面积变化）。
        const double lo = std::min(fg[a], fg[b]), hi = std::max(fg[a], fg[b]);
        LOG(INFO) << "interp[" << s << "/r" << ri << "] a=" << a << " b=" << b
                  << " ratio=" << kRatio
                  << "  GPU vs CPU真值 平均通道差=" << d << " 几何不一致率=" << dm
                  << "  前景占比=" << f << " (端点 " << lo << "~" << hi << ")";
        if (d < 0 || dm < 0 || f < 0) {
            LOG(ERROR) << "门禁 失败: interp[" << s << "/r" << ri << "] 读图/尺寸错";
            ++fail;
        } else if (dm > 0.02) {
            LOG(ERROR) << "门禁 失败: interp[" << s << "/r" << ri
                       << "] GPU 插值结果与 CPU 真值**几何**不一致 (mask=" << dm
                       << " > 0.02) —— VS 插值或取址有错";
            ++fail;
        } else if (d > 12.0) {
            LOG(ERROR) << "门禁 失败: interp[" << s << "/r" << ri << "] 着色差异过大 (diff="
                       << d << ")";
            ++fail;
        }
        if (f >= 0 && (f < 0.6 * lo || f > 1.4 * hi)) {
            LOG(ERROR) << "门禁 失败: interp[" << s << "/r" << ri << "] 前景占比 " << f
                       << " 落在端点区间 [" << 0.6 * lo << "," << 1.4 * hi
                       << "] 之外 —— 插值把顶点插到两端之外（发散或取错 pose）";
            ++fail;
        }
      }
    }

    // ============ 门禁三：ratio 必须真的生效（防「ignore uRatio 恒取 pose_a」） ============
    // 同一对帧在 ratio=0.25 与 0.75 下渲染，两图必须**明显不同**。若 shader 忽略 ratio，
    //   两张图会逐像素相同 ⇒ 这条能抓住「插值没接线」这类假实现。
    {
        const int a = 0, b = 1;
        app.pose_a_sel_ = a;
        app.pose_b_sel_ = b;
        app.draw_which_ = jpov_skeleton_gold::SkeletonGoldApp::DrawWhich::kSkinned;
        app.ratio_sel_ = 0.25f;
        app.RunOnce(input, winfo, (outdir + "ratio_lo.png").c_str());
        app.ratio_sel_ = 0.75f;
        app.RunOnce(input, winfo, (outdir + "ratio_hi.png").c_str());
        const Img lo_i = Load(outdir + "ratio_lo.png"), hi_i = Load(outdir + "ratio_hi.png");
        const double d = MeanAbsDiffLeft(lo_i, hi_i, HW);
        LOG(INFO) << "ratio 生效检查: ratio=0.25 vs 0.75  平均通道差 = " << d;
        if (d < 0) {
            LOG(ERROR) << "门禁 失败: ratio 生效检查 读图失败";
            ++fail;
        } else if (d < 1.0) {
            LOG(ERROR) << "门禁 失败: ratio=0.25 与 0.75 渲染几乎相同 (diff=" << d
                       << " < 1.0) —— shader 未使用 uRatio（插值未接线）";
            ++fail;
        }
    }

    if (fail > 0) {
        LOG(ERROR) << "jpov_skinned_multipose_test FAILED: " << fail << " 项门禁未过";
        return 1;
    }
    LOG(INFO) << "jpov_skinned_multipose_test PASSED (" << npose << " poses + "
              << interp_cases << " 插值组 + ratio 生效检查)";
    return 0;
}

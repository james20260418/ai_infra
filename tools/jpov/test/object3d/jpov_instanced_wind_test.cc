// JPOV 静态实例路径的**逐实例风参数**像素校验（GPU）。
//
// 守的是什么（2026-10-27）：
//   InstanceState 的逐实例风字段（loc11 = 风向.xyz + 吹倒比例，loc15 = 相位 / 摆幅 /
//   频率 / 阵风相位）必须**真的逐实例**生效，而不是被当成"整批一个值"：
//     1) 两个实例参数完全相同 ⇒ 它们的像素**逐字节相同**；
//     2) 只改右侧实例的某个字段 ⇒ **右侧像素变、左侧逐字节不变**。
//   「左侧不变」是关键的那一半断言：它同时排除了「其实是全批生效」和「改了 uniform
//   影响到别的实例」两类错误 —— 如果只断言"两图不同"，一个全局 uniform 改动也能让它通过。
//
// 场景：两根细长春带柱（同一个 mesh 摆两份，x=±2），关节链自下而上 4 环，
//   风吹向 +X ⇒ 柱顶向 +X 偏。相机正对 +Z，只看轮廓。无太阳/无地面/无阴影、
//   ambient 单一色 ⇒ 像素变化只可能来自几何位移（不受光照/材质干扰）。
//
// 五个 pass（每个 pass 只改一处）：
//   A 基准（两实例同参）      B 重复 A（确定性）
//   C 右实例相位 0.25s        D 右实例摆幅倍数 0
//   E 右实例风向翻转 −X       F 右实例频率倍数 2
// 断言：A≡B（全图逐字节）；C/D/E/F 各自「左半逐字节不变 + 右半有足量像素变化」。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

constexpr int kResW = 1280;
constexpr int kResH = 720;
// 两实例的世界 x（相机 (0,2,10)、fov 60 ⇒ 屏幕中线在 640）。
constexpr float kInstX[2] = {-2.0f, 2.0f};
// 世界 x = ±2 落在像素中线两侧 ⇒ 用 640 切左右半即可，无需精确换算。
constexpr int kSplitX = kResW / 2;

// 一根 4 米高、0.24 米宽的**分片**春带柱：9 排顶点（y = 0.0, 0.5, … 4.0），法线 +Z。
//   为什么必须分片：一个只有顶/底两排顶点的盒子在关节表里只有一个刚性段，
//   枢轴链转起来整根是刚体，看不出「弯曲」；分片后每个高度归不同关节，
//   链式旋转才会让柱子弯出一道弧。
jpov::MeshData MakeWindColumn() {
    jpov::MeshData m;
    constexpr int kRows = 9;              // y = 0, 0.5, ..., 4.0
    constexpr int kJointCount = 4;        // 关节：y = 0,1,2,3（level 0..3）
    constexpr float kHalfW = 0.12f;
    for (int r = 0; r < kRows; ++r) {
        const float y = 0.5f * static_cast<float>(r);
        m.positions.push_back({-kHalfW, y, 0.0f});
        m.positions.push_back({kHalfW, y, 0.0f});
        m.normals.push_back({0.0f, 0.0f, 1.0f});
        m.normals.push_back({0.0f, 0.0f, 1.0f});
    }
    for (int r = 0; r + 1 < kRows; ++r) {
        const uint32_t a = static_cast<uint32_t>(2 * r);
        const uint32_t b = a + 1;
        const uint32_t c = a + 2;
        const uint32_t d = a + 3;
        m.indices.insert(m.indices.end(), {a, b, d, a, d, c});
    }

    // 关节表：pivot 在 (0, level, 0)，父链自根向上。
    for (int j = 0; j < kJointCount; ++j) {
        jpov::MeshData::WindJoint wj{};
        wj.pivot[0] = 0.0f;
        wj.pivot[1] = static_cast<float>(j);
        wj.pivot[2] = 0.0f;
        wj.parent = (j == 0) ? -1.0f : static_cast<float>(j - 1);
        wj.level = static_cast<float>(j);
        wj.phase = 0.0f;   // 逐关节相位留 0：本测试只让**逐实例**相位产生差异
        wj.soft = 0.5f;
        m.wind_joints.push_back(wj);
    }
    // 逐顶点归属：取"不高于本顶点高度的最高关节"（y=0..3.4 → 关节 0..3）。
    for (int r = 0; r < kRows; ++r) {
        const float y = 0.5f * static_cast<float>(r);
        const int j = std::min(kJointCount - 1, static_cast<int>(std::floor(y)));
        m.wind_link.push_back(jpov::Vec2f{static_cast<float>(j), 0.0f});
        m.wind_link.push_back(jpov::Vec2f{static_cast<float>(j), 0.0f});
    }

    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));
    m.Validate();
    return m;
}

// 一个 pass = 一组逐实例参数。基准 pass（kBaseline）两份实例完全一致。
enum class Pass {
    kBaseline,        // 两实例同参
    kBaselineRepeat,  // 与 kBaseline 完全相同（确定性对照）
    kRightPhase,      // 右实例相位偏移
    kRightAmpZero,    // 右实例摆幅倍数 = 0
    kRightDirFlip,    // 右实例风向反了 180°
    kRightFreq2,      // 右实例频率倍数 = 2
};

jpov::InstanceState MakeInstance(int which, Pass pass) {
    jpov::InstanceState st;
    st.transform.center = {kInstX[which], 0.0f, 0.0f};
    st.transform.up = {0.0f, 1.0f, 0.0f};
    st.transform.front = {0.0f, 0.0f, 1.0f};
    st.transform.scale = 1.0f;
    st.wind_phase = 0.0f;
    st.wind_amp_scale = 1.0f;
    st.wind_freq_scale = 1.0f;
    st.wind_gust_phase_cycles = 0.0f;

    if (which != 1) {
        return st;   // 左实例在所有 pass 里都保持不变（"左侧逐字节不变"断言的依据）
    }
    switch (pass) {
        case Pass::kBaseline:
        case Pass::kBaselineRepeat:
            break;
        case Pass::kRightPhase:
            st.wind_phase = 0.25f;
            break;
        case Pass::kRightAmpZero:
            st.wind_amp_scale = 0.0f;
            break;
        case Pass::kRightDirFlip:
            st.wind_direction = jpov::Vec3f{-1.0f, 0.0f, 0.0f};
            break;
        case Pass::kRightFreq2:
            st.wind_freq_scale = 2.0f;
            break;
    }
    return st;
}

class InstancedWindApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t mesh_id = 0;
    Pass pass = Pass::kBaseline;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_ = static_cast<float>(kResW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kResH);
        cmds->camera.position = {0.0f, 2.0f, 10.0f};
        cmds->camera.target = {0.0f, 2.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;

        // 纯 ambient、无色偏：像素变化只能是几何位移（不受光照/材质干扰）。
        cmds->ambient = jpov::AmbientLight{.color = {1.0f, 1.0f, 1.0f, 1.0f},
                                           .intensity = 1.0f};
        cmds->tone_mapping = false;

        jpov::WindCommand w;
        w.time = 0.35f;          // 钉死（确定性；否则两次渲染必不相同）
        w.amplitude = 1.0f;
        w.frequency = 0.5f;
        w.direction = jpov::Vec3f{1.0f, 0.0f, 0.0f};
        w.turb_amplitude = 0.0f;  // 关掉湍流：本测试只看枢轴链 + 逐实例参数
        w.gust_strength = 0.0f;

        std::vector<jpov::InstanceState> instances = {
            MakeInstance(0, pass), MakeInstance(1, pass)};
        const jpov::PBRMaterial mat = jpov::PBRMaterial::SolidColor(jpov::kColorWhite);
        cmds->DrawInstancedObject(mesh_id, mat, std::move(instances),
                                  /*highlight*/ false, w);
    }
};

// 该半区里「非背景」像素数（任一通道 > 20；本例是纯 ambient 白柱、背景近黑）。
//   用途：**证明这一半真的画了东西**。没有它，"左半逐字节不变"可能是恒真的
//   （左边根本没有几何可动）—— 那正是自审要抓的无效断言。
int CountVisible(const unsigned char* rgb, int x0, int x1) {
    int c = 0;
    for (int y = 0; y < kResH; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t p = (static_cast<size_t>(y) * kResW + x) * 3;
            const int m = std::max(std::max(rgb[p + 0], rgb[p + 1]), rgb[p + 2]);
            if (m > 20) {
                ++c;
            }
        }
    }
    return c;
}

// 半区像素差：count = 该半区里任一通道差 > tol 的像素数，mx = 最大单通道差。
struct HalfDiff {
    int count = 0;
    int mx = 0;
};

HalfDiff DiffHalf(const unsigned char* a, const unsigned char* b, int x0, int x1, int tol) {
    HalfDiff d;
    for (int y = 0; y < kResH; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t p = (static_cast<size_t>(y) * kResW + x) * 3;
            const int dr = std::abs(static_cast<int>(a[p + 0]) - static_cast<int>(b[p + 0]));
            const int dg = std::abs(static_cast<int>(a[p + 1]) - static_cast<int>(b[p + 1]));
            const int db = std::abs(static_cast<int>(a[p + 2]) - static_cast<int>(b[p + 2]));
            const int m = std::max(std::max(dr, dg), db);
            if (m > tol) {
                ++d.count;
            }
            d.mx = std::max(d.mx, m);
        }
    }
    return d;
}

struct Image {
    std::vector<unsigned char> rgb;
    bool valid = false;
};

Image LoadRgb(const std::string& path) {
    Image img;
    int w = 0;
    int h = 0;
    int comps = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comps, 3);
    if (data == nullptr) {
        LOG(ERROR) << "读回渲染图失败: " << path;
        return img;
    }
    CHECK_EQ(w, kResW) << path;
    CHECK_EQ(h, kResH) << path;
    img.rgb.assign(data, data + static_cast<size_t>(kResW) * kResH * 3);
    img.valid = true;
    stbi_image_free(data);
    return img;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_instanced_wind_test/";
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "JPOV Instanced Wind Test";
    cfg.headless = true;
    InstancedWindApp app(cfg);
    app.Init();
    app.mesh_id = app.RegisterMesh(MakeWindColumn());

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kResW);
    winfo.height = static_cast<float>(kResH);
    jpov::InputSnapshot input{};

    auto render = [&](Pass p, const char* name) {
        app.pass = p;
        const std::string path = outdir + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        return LoadRgb(path);
    };

    const Image base = render(Pass::kBaseline, "a_baseline");
    const Image base2 = render(Pass::kBaselineRepeat, "b_baseline_repeat");
    const Image phase = render(Pass::kRightPhase, "c_right_phase");
    const Image amp0 = render(Pass::kRightAmpZero, "d_right_amp0");
    const Image dirflip = render(Pass::kRightDirFlip, "e_right_dirflip");
    const Image freq2 = render(Pass::kRightFreq2, "f_right_freq2");
    app.Finalize();

    if (!base.valid || !base2.valid || !phase.valid || !amp0.valid || !dirflip.valid ||
        !freq2.valid) {
        LOG(ERROR) << "TEST FAILED: 有渲染图读不回（" << outdir << "）";
        return 1;
    }

    bool ok = true;

    // ---- 前置：左右两侧**都必须真的画出了柱子** ----
    //   否则后面「左半不变」就是恒真断言（左边是空白，当然怎么改都不变）。
    const int vis_left = CountVisible(base.rgb.data(), 0, kSplitX);
    const int vis_right = CountVisible(base.rgb.data(), kSplitX, kResW);
    LOG(INFO) << "可见像素: 左半 = " << vis_left << "，右半 = " << vis_right;
    if (vis_left < 200 || vis_right < 200) {
        LOG(ERROR) << "两侧柱子没都画出来（左 " << vis_left << " / 右 " << vis_right
                   << "）⇒ 后面的隔离断言会退化成恒真，测试无意义";
        ok = false;
    }

    // ---- 0) 确定性：同参两帧必须全图逐字节相同 ----
    //   否则后面的"差异"可能来自噪声，整套断言就失去意义。
    const HalfDiff det = DiffHalf(base.rgb.data(), base2.rgb.data(), 0, kResW, 0);
    LOG(INFO) << "确定性: 同参两帧 diff 像素 = " << det.count << "（最大 " << det.mx << "）";
    if (det.count != 0) {
        LOG(ERROR) << "同参两帧不一致（" << det.count << " 像素），无法作为基准";
        ok = false;
    }

    // ---- 1) 每项逐实例参数：右侧变化 + 左侧逐字节不变 ----
    struct Case {
        const char* name;
        const Image* img;
        int min_right_pixels;   // 右半至少要有这么多像素变化（下面的实测数量级留了余量）
    };
    // 阈值说明（实测值见日志）：柱子只有 0.24 米宽（屏幕上 ~15 列）、4 米高，
    // 而"变化"只发生在轮廓边缘（柱顶偏移几个像素），所以变化像素数是**千级**而不是
    // 万级。实测（2026-10-27 llvmpipe）：相位 1313 / 摆幅 1258 / 风向 2491 / 频率 1270。
    // 阈值取实测的一半左右：既能抓住"字段没接上"（数量会掉到 0），也不至于被细节抖动搞红。
    const Case cases[] = {
        {"相位 phase", &phase, 400},
        {"摆幅 amp_scale", &amp0, 400},
        {"风向 direction", &dirflip, 900},
        {"频率 freq_scale", &freq2, 400},
    };
    for (const Case& c : cases) {
        const HalfDiff left = DiffHalf(base.rgb.data(), c.img->rgb.data(), 0, kSplitX, 0);
        const HalfDiff right =
            DiffHalf(base.rgb.data(), c.img->rgb.data(), kSplitX, kResW, 8);
        LOG(INFO) << c.name << ": 左半变化像素 = " << left.count << "（最大 " << left.mx
                  << "），右半变化像素(>8) = " << right.count << "（最大 " << right.mx << "）";
        if (left.count != 0) {
            LOG(ERROR) << c.name << "：左实例不该受任何影响，但左半有 " << left.count
                       << " 像素变了 ⇒ 逐实例隔离被破坏（改的其实是整批）";
            ok = false;
        }
        if (right.count < c.min_right_pixels) {
            LOG(ERROR) << c.name << "：右实例变了但右半只有 " << right.count
                       << " 像素差异（期望 ≥ " << c.min_right_pixels
                       << "）⇒ 该字段没有真正接到 shader，或幅度被吃掉";
            ok = false;
        }
    }

    if (!ok) {
        LOG(ERROR) << "TEST FAILED: 逐实例风参数（图: " << outdir << "）";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: 逐实例风参数（相位/摆幅/风向/频率 各自只影响自己那一份）";
    return 0;
}

// JPOV postprocess — SMAA 后处理抗锯齿的行为锚点 test（headless 渲染）。
//
// 目标：验证 cmds.smaa 在端到端渲染链路上**生效且只作用于边缘**，且确定性。
//
// 场景：纯色几何（DrawTriangle3D，无 PBR 光照 → 不受 llvmpipe 抖动影响）。
//   一块亮色直角三角形盖在黑背景上，斜边是一条**强烈锯齿**的长边。
//
// 断言（几何无关的鲁棒不变量，不依赖 SMAA 精确权重）：
//   1. SMAA on 与 off 存在差异（> 0 像素变化）—— 证明它真的在动。
//   2. 变化是**局部的**：变化像素占比 < 15%（不是整幅糊掉/位移）。
//   3. 远离边缘的像素**完全不变**：凡「距任何变化像素 > 半径 R」的像素，
//      on 与 off 逐字节相等 —— 捕捉「全图被改」「整体位移/变色」这类破坏。
//   4. 确定性：连渲两次 on，输出逐字节相同。
//
// 说明：用「远离任何变化像素」而非几何位置判据，免去对三角投影形状的假设。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

// 远离边缘判据的半径（像素）：变化像素外扩 R 后仍为 0 的像素视为「内部像素」。
constexpr int kFarRadius = 12;

const jpov::Vec3f kTriVerts[3] = {
    {-1.0f, -1.0f, 0.0f},
    { 1.0f, -1.0f, 0.0f},
    { 1.0f,  1.0f, 0.0f},
};

class SmaaApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetSmaa(bool on, float threshold = 0.1f) {
        smaa_on_ = on;
        smaa_threshold_ = threshold;
    }

    void OneIteration(int64_t,
                      const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = 640.0f;
        cmds->camera.fbo_3d_height_ = 360.0f;
        cmds->camera.position = {0.0f, 0.0f, 3.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->tone_mapping = true;
        cmds->srgb_encode = true;

        if (smaa_on_) {
            jpov::SmaaConfig sc;
            sc.enabled = true;
            sc.threshold = smaa_threshold_;
            cmds->smaa = sc;
        }

        // 亮色三角形（斜边 = 长锯齿边）盖在黑背景上。
        const jpov::Color bright = {0.8f, 0.8f, 0.8f, 1.0f};
        cmds->DrawTriangle3D(kTriVerts[0], kTriVerts[1], kTriVerts[2], bright);
    }

private:
    bool smaa_on_ = false;
    float smaa_threshold_ = 0.1f;
};

bool RenderOne(SmaaApp* app, const std::string& outpath, bool smaa_on,
               int* w, int* h, std::vector<unsigned char>* px /*output*/) {
    app->SetSmaa(smaa_on);
    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app->RunOnce(input, winfo, outpath.c_str());

    int req = 4;
    unsigned char* p = stbi_load(outpath.c_str(), w, h, &req, req);
    if (!p) {
        LOG(ERROR) << "Failed to load rendered " << outpath;
        return false;
    }
    px->assign(p, p + (*w) * (*h) * req);
    stbi_image_free(p);
    return true;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_smaa_test/";
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "SMAA Test";
    cfg.headless = true;
    SmaaApp app(cfg);
    app.Init();

    int w = 0, h = 0, w2 = 0, h2 = 0;
    std::vector<unsigned char> off_px, on_px, on_px2;
    CHECK(RenderOne(&app, outdir + "off.png", false, &w, &h, &off_px));
    CHECK(RenderOne(&app, outdir + "on.png",  true,  &w, &h, &on_px));
    CHECK(RenderOne(&app, outdir + "on2.png", true,  &w2, &h2, &on_px2));
    app.Finalize();

    CHECK_EQ(w, w2);
    CHECK_EQ(h, h2);
    const int npix = w * h;

    // ---- 断言 4：确定性（两次 on 逐字节相同）----
    CHECK(on_px == on_px2) << "SMAA 输出不确定（两次 on 渲染不同）";
    LOG(INFO) << "PASS: SMAA 输出确定性（两次 on 一致）";

    // ---- 变化掩膜 ----
    std::vector<unsigned char> changed(npix, 0);
    long changed_cnt = 0;
    for (int i = 0; i < npix; ++i) {
        bool ch = false;
        for (int c = 0; c < 4; ++c) {
            if (off_px[i * 4 + c] != on_px[i * 4 + c]) {
                ch = true;
                break;
            }
        }
        if (ch) {
            changed[i] = 1;
            ++changed_cnt;
        }
    }

    // ---- 断言 1：SMAA 确实改变了一些像素 ----
    CHECK_GT(changed_cnt, 0) << "SMAA on/off 完全相同（未生效？）";
    LOG(INFO) << "PASS: SMAA 改变了 " << changed_cnt << " 像素";

    // ---- 断言 2：变化是局部的 ----
    const double changed_frac = static_cast<double>(changed_cnt) / npix;
    CHECK_LT(changed_frac, 0.15)
        << "SMAA 变化像素占比过高（" << changed_frac << "），疑似整幅糊掉/位移";
    LOG(INFO) << "PASS: 变化占比 " << changed_frac << " < 0.15（局部）";

    // ---- 断言 3：远离变化的像素完全不变 ----
    // 用积分图统计每个像素 (2R+1)² 窗口内的变化计数，计数为 0 → 该像素「远离边缘」。
    std::vector<long> sat(static_cast<size_t>(w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        long row = 0;
        for (int x = 0; x < w; ++x) {
            row += changed[y * w + x];
            sat[(y + 1) * (w + 1) + (x + 1)] = sat[y * (w + 1) + (x + 1)] + row;
        }
    }
    auto window_sum = [&](int x0, int y0, int x1, int y1) -> long {
        // 半开区间 [x0,x1) x [y0,y1)
        return sat[y1 * (w + 1) + x1] - sat[y0 * (w + 1) + x1]
             - sat[y1 * (w + 1) + x0] + sat[y0 * (w + 1) + x0];
    };

    long far_cnt = 0, far_mismatch = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - kFarRadius);
            const int y0 = std::max(0, y - kFarRadius);
            const int x1 = std::min(w, x + kFarRadius + 1);
            const int y1 = std::min(h, y + kFarRadius + 1);
            if (window_sum(x0, y0, x1, y1) != 0) {
                continue;  // 靠近边缘，跳过
            }
            ++far_cnt;
            const unsigned char* o = &off_px[(y * w + x) * 4];
            const unsigned char* n = &on_px[(y * w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (o[c] != n[c]) {
                    ++far_mismatch;
                    break;
                }
            }
        }
    }
    CHECK_GT(far_cnt, npix / 2) << "远离边缘像素过少（" << far_cnt << "），判据异常";
    CHECK_EQ(far_mismatch, 0)
        << "存在 " << far_mismatch << " 个远离边缘的像素被 SMAA 改动"
           "（AA 不应影响平坦内部；疑似全图被改/位移）";
    LOG(INFO) << "PASS: " << far_cnt << " 个远离边缘像素全部不变（改动仅限边缘）";

    LOG(INFO) << "TEST PASSED: SMAA 生效、局部、确定性";
    return 0;
}

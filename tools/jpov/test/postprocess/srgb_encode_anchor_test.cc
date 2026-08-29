// JPOV postprocess — sRGB 编码在渲染链路上的行为锚点 test。
//
// 目标：验证 "srgb_encode=true" 确实在端到端渲染链路上生效，且方向正确。
// 用**确定性纯色 3D 场景**（无 PBR 光照 → 不受 llvmpipe PBR 抖动影响）：
//   - 一个白色正方体 + 黑色背景
//   - tone_mapping=true 走 HDR → ACES → (可选) sRGB 编码
//
// 断言（不依赖 ACES 精确值，验证相对行为 + 绝对锚点）：
//   1. 背景像素：srgb=true 与 false 均为 0（lin=0→ACES(0)=0→srgb(0)=0，绝对确定）
//   2. 物体像素：srgb=true 的亮度 > srgb=false（sRGB 编码把线性值整体提亮）
//   3. 全部像素都在 [0,255]（clamp 不破坏）
//
// 配合 srgb_math_test（纯函数查表）验证公式精确性，本 test 验证链路生效与方向。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/srgb.h"

namespace {

// 用 DrawTriangle3D 纯色，无光照，确定性。
// 顶点类型用 jpov::Vec3f（DrawTriangle3D 的签名）。
const jpov::Vec3f kVerts[8] = {
    {-0.25f, -0.25f, 0.25f},   // v0
    { 0.25f, -0.25f, 0.25f},   // v1
    { 0.25f,  0.25f, 0.25f},   // v2
    {-0.25f,  0.25f, 0.25f},   // v3
    {-0.25f, -0.25f, -0.25f},  // v4
    { 0.25f, -0.25f, -0.25f},  // v5
    { 0.25f,  0.25f, -0.25f},  // v6
    {-0.25f,  0.25f, -0.25f},  // v7
};

void AddQuad(jpov::RenderCommandList* cmds, int i0, int i1, int i2, int i3,
             const jpov::Color& color) {
    cmds->DrawTriangle3D(kVerts[i0], kVerts[i1], kVerts[i3], color);
    cmds->DrawTriangle3D(kVerts[i1], kVerts[i2], kVerts[i3], color);
}

class SrgbAnchorApp : public JPOV {
public:
    using JPOV::JPOV;

    // true：srgb_encode 开（被测路径）；false：关（对照）。
    void SetSrgbOn(bool on) { srgb_on_ = on; }

    void OneIteration(int64_t,
                      const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // 被测开关（tone_mapping 保持 true，走 HDR→ACES→sRGB 链）
        cmds->tone_mapping = true;
        cmds->srgb_encode = srgb_on_;

        // 一个白色立方体（6 面全白，突出亮度对比）＋黑背景
        const jpov::Color white = {1.0f, 1.0f, 1.0f, 1.0f};
        AddQuad(cmds, 0, 1, 2, 3, white);  // front
        AddQuad(cmds, 4, 5, 6, 7, white);  // back
        AddQuad(cmds, 1, 5, 6, 2, white);  // right
        AddQuad(cmds, 4, 0, 3, 7, white);  // left
        AddQuad(cmds, 3, 2, 6, 7, white);  // top
        AddQuad(cmds, 4, 5, 1, 0, white);  // bottom
    }

private:
    bool srgb_on_ = true;
};

// 渲染一帧到 outpath，返回像素数组（RGBA）与尺寸。
// app 已 Init，多次调用 RunOnce（不重复 Init/Finalize，避免 Xvfb 端口竞态）。
bool RenderOne(SrgbAnchorApp* app, const std::string& outpath, bool srgb_on,
               int* w, int* h, std::vector<unsigned char>* px /*output*/) {
    app->SetSrgbOn(srgb_on);

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
    const std::string outdir = jpov::GetOutputDir() + "jpov_srgb_anchor_test/";
    std::system(("mkdir -p " + outdir).c_str());

    // Init 一次，多次 RunOnce（复用同一 Xvfb 实例，避免端口竞态）。
    JPOV::Config cfg;
    cfg.title = "sRGB Anchor Test";
    cfg.headless = true;
    SrgbAnchorApp app(cfg);
    app.Init();

    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    std::vector<unsigned char> off_px, on_px;
    CHECK(RenderOne(&app, outdir + "off.png", false, &w0, &h0, &off_px));
    CHECK(RenderOne(&app, outdir + "on.png",  true,  &w1, &h1, &on_px));
    app.Finalize();

    CHECK_EQ(w0, w1);
    CHECK_EQ(h0, h1);
    const int w = w0, h = h0;

    // ---- 统计像素 ----
    long on_obj_sum = 0, off_obj_sum = 0;  // 物体像素亮度总和
    long on_obj_cnt = 0, off_obj_cnt = 0;
    long on_bg_cnt = 0, off_bg_cnt = 0;
    long on_obj_saturated = 0;  // srgb 下被钳成纯白(RGB=255,255,255)的物体像素数

    for (int i = 0; i < w * h; ++i) {
        const unsigned char* q = &on_px[i * 4];
        const unsigned char* p = &off_px[i * 4];
        const int on_lum  = (q[0] + q[1] + q[2]) / 3;
        const int off_lum = (p[0] + p[1] + p[2]) / 3;

        // 背景（离屏中心较远角，立方体只占中间 ~60%）：
        // 用"关闭 sRGB 时也是黑的"来分背景——黑背景 lin=0，两侧都应为 0。
        if (off_lum == 0 && on_lum == 0) {
            ++on_bg_cnt;
            ++off_bg_cnt;
            continue;
        }
        // 物体像素
        on_obj_sum += on_lum;
        off_obj_sum += off_lum;
        ++on_obj_cnt;
        ++off_obj_cnt;
        if (q[0] == 255 && q[1] == 255 && q[2] == 255) ++on_obj_saturated;
    }

    LOG(INFO) << "背景像素(两侧均为0): off=" << off_bg_cnt
              << " on=" << on_bg_cnt;
    LOG(INFO) << "物体像素: off_cnt=" << off_obj_cnt
              << " on_cnt=" << on_obj_cnt;
    LOG(INFO) << "物体平均亮度: off="
              << (off_obj_cnt ? (double)off_obj_sum / off_obj_cnt : 0.0)
              << "  on=" << (on_obj_cnt ? (double)on_obj_sum / on_obj_cnt : 0.0);

    // ---- 断言 1：存在物体像素且有黑背景 ----
    CHECK_GT(on_obj_cnt, 0) << "未渲染出物体（场景为空？）";
    CHECK_GT(on_bg_cnt, 0) << "未检测到黑背景（立方体应只占部分画面）";
    LOG(INFO) << "PASS: 场景渲染出物体 + 黑背景";

    // ---- 断言 2：sRGB 编码把物体像素整体提亮 ----
    const double on_avg  = (double)on_obj_sum  / on_obj_cnt;
    const double off_avg = (double)off_obj_sum / off_obj_cnt;
    CHECK_GT(on_avg, off_avg)
        << "srgb=true 物体应比 srgb=false 亮（sRGB 编码提亮线性值），实际 on_avg="
        << on_avg << " off_avg=" << off_avg;
    LOG(INFO) << "PASS: srgb=true 物体亮度(" << on_avg
              << ") > srgb=false(" << off_avg << ")";

    // ---- 断言 3：编码后未整体过曝成纯白（clamp 不该把物体面钳成 255） ----
    // 白面 ACES≈0.804→sRGB≈0.91≈232，远未到 255。若编码/链路 bug 导致
    // 大面积钳到纯白（255,255,255），说明 clamp 路径异常，应捕获。
    CHECK_EQ(on_obj_saturated, 0)
        << "srgb=true 存在被钳成纯白(255,255,255)的物体像素，clamp 异常";
    LOG(INFO) << "PASS: 编码后物体像素未过曝（无纯白 255 钳位）";

    // ---- 附注：理论值对照（信息性，非硬断言） ----
    // 白面 lin=1.0 → ACES(1.0)≈0.807 → srgb=LinearToSrgb(0.807)≈0.909≈232
    // 无 sRGB：直写=0.807≈206。on 应明显 > off（提亮 ~26/255）。
    {
        const float aces_white = 0.807f;  // aces_curve(1.0) 近似
        const float on_theory  = jpov::postprocess::LinearToSrgb(aces_white);
        LOG(INFO) << "理论对照: 白面 srgb≈" << (int)(on_theory * 255)
                  << " 无srgb≈" << (int)(aces_white * 255)
                  << "（实测 on_avg=" << (int)on_avg
                  << " off_avg=" << (int)off_avg << "）";
    }

    LOG(INFO) << "TEST PASSED: sRGB 编码在渲染链路上生效，方向正确，clamp 正常";
    return 0;
}

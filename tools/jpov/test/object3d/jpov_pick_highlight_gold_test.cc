// JPOV picking + highlight gold test —— 验证 GPU color-ID 拾取 + 方法 B 高亮边框
//
// 验证内容：
//   1. GPU color-ID 拾取：相机正对场景，发起 pick 查询命中指定屏幕坐标上的物体，
//      JPOV::last_pick() 返回正确的 picking_id；落在背景上返回 hit=false。
//   2. 高亮纯色边框：给某物体设 highlight + highlight_style，渲染后画面出现
//      纯色边框（中心柱区域含暖黄像素，区别于未高亮）。
//
// 通过条件：
//   - 拾取结果精确（id 正确、背景 miss）
//   - 高亮区域出现金色（r>g 明显）像素
//   - gold image（高亮场景）存在供肉眼参考

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/jpov_pick_highlight_gold_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_pick_highlight_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/pick_highlight_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/object3d/pick_highlight_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());

    // 校验 gold image 存在（generator 生成，供肉眼参考；缺失=回归）。
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_pick_highlight_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV PickHighlight Test";
    cfg.headless = true;
    jpov_pick_highlight::PickHighlightApp app(cfg);
    app.Init();

    // 注册 mesh（本地坐标：MakeBox(half_front, half_up, half_left)）。
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(3.0f, 0.1f, 3.0f));
    app.center_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.6f, 1.0f, 0.6f));
    app.left_mesh_   = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 1.0f, 0.5f));
    app.right_mesh_  = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 1.0f, 0.5f));

    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    using namespace jpov_pick_highlight;

    // ---- 帧 1：点屏幕中心，应命中中心柱（kIdCenter） ----
    {
        app.pick_enabled = true;
        app.pick_x = 640.0f;   // 屏幕（窗口）中心 X
        app.pick_y = 360.0f;   // 屏幕（窗口）中心 Y
        app.highlight_id = 0;  // 本帧不高亮
        const std::string p = outdir + "frame_center.png";
        app.RunOnce(input, winfo, p.c_str());
        const jpov::PickResult& r = app.last_pick();
        LOG(INFO) << "pick center → hit=" << r.hit
                  << " picking_id=" << r.picking_id;
        CHECK(r.hit) << "中心点击应命中物体";
        CHECK_EQ(r.picking_id, kIdCenter)
            << "中心点击应命中 center(picking_id=" << kIdCenter
            << "), 实际=" << r.picking_id;
    }

    // ---- 帧 2：点左上角（纯背景），应未命中 ----
    {
        app.pick_enabled = true;
        app.pick_x = 8.0f;     // 极左极上（背景）
        app.pick_y = 8.0f;
        app.highlight_id = 0;
        const std::string p = outdir + "frame_corner.png";
        app.RunOnce(input, winfo, p.c_str());
        const jpov::PickResult& r = app.last_pick();
        LOG(INFO) << "pick corner → hit=" << r.hit
                  << " picking_id=" << r.picking_id;
        CHECK(!r.hit) << "左上角应命中背景（未命中任何物体）";
    }

    // ---- 帧 3：高亮中心柱，渲染高亮场景 ----
    {
        app.pick_enabled = false;      // 本帧只做高亮，不拾取
        app.highlight_id = kIdCenter;  // 高亮中心柱
        const std::string p = outdir + "highlight_center.png";
        app.RunOnce(input, winfo, p.c_str());
        LOG(INFO) << "highlight center rendered: " << p;
    }
    app.Finalize();

    // ---- 高亮像素检查：全图扫描暖黄（金色边框）像素 ----------------
    // llvmpipe 下 PBR 光照把物体本体压得很暗（几乎纯黑），只有高亮的金色边框
    //（r 明显 > g）是亮的，故“暖黄像素数”即“边框面积”的可靠信号。
    {
        const std::string p = outdir + "highlight_center.png";
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load highlighted PNG: " << p;

        int warm = 0;
        const int r_gap = 25;
        for (int i = 0; i < w * h; ++i) {
            const unsigned char* q = &px[i * 4];
            if (q[0] > q[1] + r_gap && q[0] > 60) ++warm;  // 排除暗色本底
        }
        stbi_image_free(px);
        LOG(INFO) << "highlight warm (gold border) pixel count = " << warm;
        CHECK_GE(warm, 500)
            << "未检测到足够金色边框像素，高亮描边未渲染";
    }

    // ---- 回归：高亮不破坏本体颜色 ----
    // 高亮只应加边框，被高亮物体内部应保持原 PBR 本体色（不能变纯黑）。
    // 往：MSAA 路径 DrawHighlightResolvedPass 第 1 步写 stencil 时未关颜色写，
    // 用未初始化的 outline shader uColor（默认黑）把高亮物体内部整片覆盖成黑。
    // 修复：第 1 步 glColorMask(GL_FALSE...) 只写 stencil，第 2 步描边前恢复。
    // 断言：高亮后中心柱内圈（中心 ±25px，避开 1.08 倍外扩边框）应存在非黑像素。
    {
        const std::string p = outdir + "highlight_center.png";
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load " << p;
        int nonblack = 0;
        const int cx = w / 2, cy = h / 2;
        const int r = 25;
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                const unsigned char* q = &px[(y * w + x) * 4];
                if (q[0] > 0 || q[1] > 0 || q[2] > 0) ++nonblack;
            }
        }
        stbi_image_free(px);
        LOG(INFO) << "highlight center 内圈非黑像素 = " << nonblack
                  << "（预期 > 0，高亮不破坏本体色）";
        CHECK_GT(nonblack, 0)
            << "高亮物体内部被染成纯黑（本体色被破坏）。检查 "
               "DrawHighlightPass 第 1 步是否禁用颜色写（glColorMask）。";
    }

    // ---- 冒烟：整体输出非空 ----
    {
        const std::string p = outdir + "highlight_center.png";
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load " << p;
        int nz = 0;
        for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
        stbi_image_free(px);
        LOG(INFO) << "highlight coverage=" << (double(nz) / (w * h) * 100.0) << "%";
        CHECK_GT(nz, 0) << "空场景";
    }

    LOG(INFO) << "TEST PASSED: picking 命中/未中正确 + 高亮边框渲染";
    return 0;
}

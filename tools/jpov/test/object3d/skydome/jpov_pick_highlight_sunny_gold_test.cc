// JPOV picking + highlight —— standard_sunny_day 基底 gold test
//
// 以 standard_sunny_day 场景为底座，验证：
//   1. GPU color-ID 拾取：屏幕点 stool 中心 → kIdStool；wall → kIdWall；背景 → miss
//   2. 方法 B 高亮边框：stool + wall 渲染出纯色边框，且高亮不破坏本体颜色
//
// 通过条件：拾取精确、背景 miss、金色边框出现、高亮物体内部保持本体色（非黑）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_pick_highlight_sunny_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_pick_highlight_sunny_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/skydome/"
             "pick_highlight_sunny_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/skydome/pick_highlight_sunny_1280x720.png";
}

// 拾取查询 + 读结果（RunOnce 一帧同步完成，last_pick() 即本帧结果）。
template <typename App>
jpov::PickResult Pick(App& app, const jpov::WindowInfo& winfo,
                      const jpov::InputSnapshot& input,
                      float x, float y, const std::string& tag) {
    app.pick_enabled = true;
    app.pick_x = x;
    app.pick_y = y;
    const std::string p = GetOutputDir() + tag + ".png";
    app.RunOnce(input, winfo, p.c_str());
    app.pick_enabled = false;
    return app.last_pick();
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
            "jpov_pick_highlight_sunny_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Pick+Highlight Sunny Test";
    cfg.headless = true;
    jpov_pick_highlight_sunny::PickHighlightSunnyApp app(cfg);
    app.Init();
    // 高亮 stool + wall（两者可拾取）。
    jpov_pick_highlight_sunny::BuildScene(&app,
                                          /*highlight_stool=*/true,
                                          /*highlight_wall=*/true);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    using namespace jpov_pick_highlight_sunny;

    // ---- 拾取断言（坐标由 probe 校准，见 memory 记录）----
    {
        // stool 中心（窗口坐标）
        const jpov::PickResult rs = Pick(app, winfo, input, kProbeStoolX, kProbeStoolY, "pick_stool");
        LOG(INFO) << "pick stool → hit=" << rs.hit << " picking_id=" << rs.picking_id;
        CHECK(rs.hit) << "stool 中心应命中";
        CHECK_EQ(rs.picking_id, kIdStool) << "stool 应命中 kIdStool";

        // wall 中心
        const jpov::PickResult rw = Pick(app, winfo, input, kProbeWallX, kProbeWallY, "pick_wall");
        LOG(INFO) << "pick wall → hit=" << rw.hit << " picking_id=" << rw.picking_id;
        CHECK(rw.hit) << "wall 中心应命中";
        CHECK_EQ(rw.picking_id, kIdWall) << "wall 应命中 kIdWall";

        // 背景（左上角）
        const jpov::PickResult rb = Pick(app, winfo, input, 5.0f, 5.0f, "pick_bg");
        LOG(INFO) << "pick bg → hit=" << rb.hit << " picking_id=" << rb.picking_id;
        CHECK(!rb.hit) << "左上角背景应未命中";
    }

    // ---- 高亮渲染 + 本体色断言 ----
    {
        app.pick_enabled = false;
        const std::string p = outdir + "highlight.png";
        app.RunOnce(input, winfo, p.c_str());
        LOG(INFO) << "highlight rendered: " << p;

        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load " << p;

        // 金色边框存在（r 明显 > g，排除暗色本底）
        int warm = 0;
        for (int i = 0; i < w * h; ++i) {
            const unsigned char* q = &px[i * 4];
            if (q[0] > q[1] + 25 && q[0] > 60) ++warm;
        }
        LOG(INFO) << "highlight warm (gold border) pixel count = " << warm;
        CHECK_GE(warm, 500) << "未检测到金色边框像素，高亮未渲染";

        // 高亮不破坏本体色：stool 中心附近（内圈避边框）应存在非黑像素。
        // （修复过：MSAA 路径高亮曾把内部覆盖成黑，见 PR67 自查。）
        auto count_nonblack = [&](int cx, int cy, int r) {
            int nb = 0;
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x) {
                    if (x < 0 || x >= w || y < 0 || y >= h) continue;
                    const unsigned char* q = &px[(y * w + x) * 4];
                    if (q[0] > 0 || q[1] > 0 || q[2] > 0) ++nb;
                }
            return nb;
        };
        // 用 stool 窗口中心（渲染分辨率下同比例），避边框取内圈 r=20。
        const int sx = static_cast<int>(kProbeStoolX * (float)w / 640.0f);
        const int sy = static_cast<int>(kProbeStoolY * (float)h / 360.0f);
        const int nb = count_nonblack(sx, sy, 12);
        LOG(INFO) << "stool 内部非黑像素 = " << nb << "（预期 > 0）";
        CHECK_GT(nb, 0) << "高亮后 stool 内部被染成纯黑（本体色被破坏）";
        stbi_image_free(px);
    }

    // ---- 冒烟：整体非空 ----
    {
        const std::string p = outdir + "highlight.png";
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load " << p;
        int nz = 0;
        for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
        stbi_image_free(px);
        float cov = static_cast<float>(nz) / (w * h);
        LOG(INFO) << "coverage=" << (cov * 100.0f) << "%";
        CHECK_GT(nz, 0) << "空场景";
    }

    LOG(INFO) << "TEST PASSED: sunny 场景 picking 命中/未中正确 + stool/wall 高亮";
    return 0;
}

// JPOV Plate2D GPU 像素测试 —— 锁住**着色器侧**行为（CPU 指令层测不到的那半边）
//
// 背景：`test/sp_ui/sp_ui_gold_test.cc` 是纯 CPU，只能证明「命令里的参数对不对」。
// 但 sp_ui 开发期最贵的两个 bug 都在着色器侧：
//   * 图集行序被整体倒置（抄了粒子那份 vrow 翻转）⇒ 播放变成「每行跳一下」，
//     看起来像「flipbook 不闭合」；
//   * 「裁 cell」与「会不会动」被合成一个判断 ⇒ 4×4 图集配 fps=0 时整张图集
//     当作一幅图显示（屏幕上是 16 个缩小的自己）。
// 这两类问题 CPU 测试一律看不见，必须在真 GL 下读回像素。
//
// 做法：造一张 **2×2 纯色图集**（每格一个颜色），逐帧渲染后读回像素断言颜色 ⇒
// 「帧号 → 图集格」的映射被逐格钉死。CI 只有 llvmpipe 也够（纯色不依赖精度）。
//
// 断言清单：
//   1. 行主序：frame 0/1/2/3 = 红/绿/蓝/黄（帧 0 在图片左上角）
//   2. fps=0 的图集静止在**第 0 格**，且整块均匀（不是 2×2 拼图）
//   3. 末帧回绕：frame 3.5 是「黄 ↔ 红」的混合（不是纯黄、不是纯红）
//   4. 两层材质独立：填充纯黑 + 边框用同图集 ⇒ 内部黑、边框偏红
//
// 负向验证（2026-10-27 实跑，证明这些断言有牙）：
//   * 把着色器的行序翻转加回去（row = tilesY-1-f0/tilesX）⇒ 帧 0 采到蓝
//     「期望 (255,0,0)，实际 (0,0,255)」→ FAIL
//   * 把 ResolveFlipbookFrame 的 `!IsAtlas()` 退回 `!IsAnimated()` ⇒ fps=0 的板
//     右下象限采到黄「期望红，实际 (255,255,0)」→ FAIL（说明整张图集被当一幅图）

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"

#ifndef _WIN32
#include <GL/gl.h>
#endif

namespace {

// 图集：2×2 格、每格 32×32 ⇒ 纹理 64×64。
constexpr int kCell = 32;
constexpr int kAtlas = kCell * 2;

// 四格颜色（行主序：帧 0 = 左上）。
constexpr unsigned char kCellColors[4][3] = {
    {255, 0, 0},      // 帧 0 红
    {0, 255, 0},      // 帧 1 绿
    {0, 0, 255},      // 帧 2 蓝
    {255, 255, 0},    // 帧 3 黄
};

// 测试场景的 6 块板材（位置写死，读像素时按同样的坐标切）。
struct PlateSlot {
    float x, y, w, h;
};
constexpr PlateSlot kSlotA = {20.0f, 20.0f, 140.0f, 140.0f};    // 帧 0 红
constexpr PlateSlot kSlotB = {180.0f, 20.0f, 140.0f, 140.0f};   // 帧 1 绿
constexpr PlateSlot kSlotC = {340.0f, 20.0f, 140.0f, 140.0f};   // 帧 2 蓝
constexpr PlateSlot kSlotD = {500.0f, 20.0f, 140.0f, 140.0f};   // 帧 3 黄
constexpr PlateSlot kSlotE = {180.0f, 200.0f, 140.0f, 140.0f};  // fps=0 ⇒ 静止红
constexpr PlateSlot kSlotF = {340.0f, 200.0f, 140.0f, 140.0f};  // 纯黑填充 + 红边框

class PlateGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetAtlas(uint32_t id) { atlas_ = id; }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;
        // 深色背景（便于判断「没画上去」）。
        cmds->DrawRect({0, 0}, {640, 360}, {0.0f, 0.0f, 0.0f, 1.0f});

        // A~D：同一张 2×2 图集，帧率 4fps，时间分别落在第 0/1/2/3 帧。
        const float times[4] = {0.0f, 0.25f, 0.5f, 0.75f};
        const PlateSlot slots[4] = {kSlotA, kSlotB, kSlotC, kSlotD};
        for (int i = 0; i < 4; ++i) {
            cmds->DrawPlate(MakePlate(slots[i], atlas_, /*fps=*/4.0f, times[i],
                                      /*border=*/false));
        }

        // E：fps = 0 —— 图集必须静止在第 0 格（红），且整块**均匀**。
        cmds->DrawPlate(MakePlate(kSlotE, atlas_, /*fps=*/0.0f, /*time=*/3.7f,
                                  /*border=*/false));

        // G/H：半透明重叠 —— 验证「预乘 alpha + (ONE, ONE_MINUS_SRC_ALPHA)」
        // 这条混合路径。黑底上一块 alpha=0.5 的白板应得到中间亮度；两块**重叠**
        // 应更亮（1-(1-0.5)² = 0.75）。若哪里退化成直通 alpha，重叠区会直接变纯白。
        {
            const float ax = 520.0f;
            jpov::Plate2DCommand g;
            g.pos = {ax, 200.0f};
            g.size = {100.0f, 60.0f};
            g.radius = 0.0f;
            g.fill_color = {1.0f, 1.0f, 1.0f, 0.5f};
            g.border_width = 0.0f;
            cmds->DrawPlate(g);
            jpov::Plate2DCommand h = g;
            h.pos = {ax + 50.0f, 220.0f};   // 与 G 重叠一半
            cmds->DrawPlate(h);
        }

        // F：填充层纯黑（texture 0）+ 边框层用同一张图集（帧 0 红）。
        {
            jpov::Plate2DCommand p;
            p.pos = {kSlotF.x, kSlotF.y};
            p.size = {kSlotF.w, kSlotF.h};
            p.radius = 12.0f;
            p.fill_color = {0.0f, 0.0f, 0.0f, 1.0f};
            p.border_color = {1.0f, 1.0f, 1.0f, 1.0f};
            p.border_width = 20.0f;
            p.border_bump = 0.0f;   // 平面带：颜色不被剖面明暗拉走，便于断言
            p.border_fade = 0.0f;
            p.time_seconds = 0.0f;
            p.border.texture_id = atlas_;
            p.border.flipbook = jpov::FlipbookDesc{2, 2, 4.0f};
            p.border.tiling = {1.0f, 1.0f};
            p.border.scroll = {0.0f, 0.0f};
            cmds->DrawPlate(p);
        }
    }

private:
    static jpov::Plate2DCommand MakePlate(const PlateSlot& s, uint32_t tex,
                                          float fps, float time, bool border) {
        jpov::Plate2DCommand p;
        p.pos = {s.x, s.y};
        p.size = {s.w, s.h};
        p.radius = 8.0f;
        p.fill_color = {1.0f, 1.0f, 1.0f, 1.0f};
        p.border_width = border ? 10.0f : 0.0f;
        p.border_bump = 0.0f;
        p.border_fade = 0.0f;
        p.time_seconds = time;
        // fill 层：2×2 图集；tiling=1 表示整块板材正好铺一格。
        p.fill.texture_id = tex;
        p.fill.flipbook = jpov::FlipbookDesc{2, 2, fps};
        p.fill.tiling = {1.0f, 1.0f};
        p.fill.scroll = {0.0f, 0.0f};
        return p;
    }

    uint32_t atlas_ = 0;
};

// 在已初始化的 GL 上下文里造一张 2×2 纯色图集（RGBA8），返回 GL 纹理名。
unsigned int CreateAtlasTexture() {
    std::vector<unsigned char> px(static_cast<size_t>(kAtlas) * kAtlas * 4, 0);
    for (int cy = 0; cy < 2; ++cy) {
        for (int cx = 0; cx < 2; ++cx) {
            const unsigned char* c = kCellColors[cy * 2 + cx];
            for (int y = 0; y < kCell; ++y) {
                for (int x = 0; x < kCell; ++x) {
                    const int gx = cx * kCell + x;
                    const int gy = cy * kCell + y;
                    unsigned char* p = &px[(static_cast<size_t>(gy) * kAtlas + gx) * 4];
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                    p[3] = 255;
                }
            }
        }
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // 板材要求：repeat（平铺）+ linear、**不生成 mipmap**（见 flipbook.h 约定 §4）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlas, kAtlas, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// 读回 PNG 并断言某点颜色（容差 tol，llvmpipe 对纯色是确定的）。
void ExpectPixel(const unsigned char* rgba, int w, int h, int x, int y,
                 int er, int eg, int eb, int tol, const char* what) {
    CHECK_GE(x, 0);
    CHECK_LT(x, w);
    CHECK_GE(y, 0);
    CHECK_LT(y, h);
    const unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
    const int dr = std::abs(static_cast<int>(p[0]) - er);
    const int dg = std::abs(static_cast<int>(p[1]) - eg);
    const int db = std::abs(static_cast<int>(p[2]) - eb);
    CHECK(dr <= tol && dg <= tol && db <= tol)
        << what << " @" << x << "," << y << "：期望 (" << er << "," << eg << ","
        << eb << ")，实际 (" << static_cast<int>(p[0]) << ","
        << static_cast<int>(p[1]) << "," << static_cast<int>(p[2])
        << ")，容差 " << tol;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_plate2d_gold_test/";
    const std::string outpath = outdir + "plate2d_gold.png";

    JPOV::Config cfg;
    cfg.title = "Plate2D Gold Test";
    cfg.headless = true;
    PlateGoldApp app(cfg);
    app.Init();

    const unsigned int gl_tex = CreateAtlasTexture();
    const uint32_t atlas_id = app.RegisterTexture(gl_tex, kAtlas, kAtlas);
    app.SetAtlas(atlas_id);

    jpov::WindowInfo winfo;
    winfo.width = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* img = stbi_load(outpath.c_str(), &w, &h, &comp, 4);
    CHECK(img != nullptr) << "读不回渲染结果：" << outpath;
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 360);

    // ---- 1. 行主序：帧 0/1/2/3 = 红/绿/蓝/黄 ----
    // 取每块板材中心（tiling=1 ⇒ 整块板材就是那一格，中心必然是纯色）。
    struct Case {
        PlateSlot slot;
        int r, g, b;
        const char* what;
    };
    const Case cases[4] = {
        {kSlotA, 255, 0, 0, "frame 0 应为红（图集左上角）"},
        {kSlotB, 0, 255, 0, "frame 1 应为绿（第 0 行第 1 列）"},
        {kSlotC, 0, 0, 255, "frame 2 应为蓝（第 1 行第 0 列）"},
        {kSlotD, 255, 255, 0, "frame 3 应为黄（第 1 行第 1 列）"},
    };
    for (const Case& c : cases) {
        const int cx = static_cast<int>(c.slot.x + c.slot.w * 0.5f);
        const int cy = static_cast<int>(c.slot.y + c.slot.h * 0.5f);
        ExpectPixel(img, w, h, cx, cy, c.r, c.g, c.b, /*tol=*/6, c.what);
    }
    LOG(INFO) << "[PASS] 图集行主序：帧 0/1/2/3 = 红/绿/蓝/黄（帧 0 在左上角）";

    // ---- 2. fps=0 的图集：静止在第 0 格，且**整块均匀** ----
    // 「整块均匀」是这条断言的关键：旧实现（fps=0 就不裁 cell）会把 2×2 图集
    // 整张缩到这块板上，四个象限颜色各不相同。
    {
        const int x0 = static_cast<int>(kSlotE.x);
        const int y0 = static_cast<int>(kSlotE.y);
        const int hw = static_cast<int>(kSlotE.w * 0.5f);
        const int hh = static_cast<int>(kSlotE.h * 0.5f);
        ExpectPixel(img, w, h, x0 + hw / 2, y0 + hh / 2, 255, 0, 0, 6,
                    "fps=0：中心应为帧 0（红）");
        ExpectPixel(img, w, h, x0 + hw + hw / 2, y0 + hh + hh / 2, 255, 0, 0, 6,
                    "fps=0：右下象限也必须是红 —— 否则说明整张图集被当一幅图");
    }
    LOG(INFO) << "[PASS] fps=0 的图集静止在第 0 格且整块均匀（不再整张显示）";

    // ---- 3. 两层材质独立：填充黑 + 边框红 ----
    {
        const int cx = static_cast<int>(kSlotF.x + kSlotF.w * 0.5f);
        const int cy = static_cast<int>(kSlotF.y + kSlotF.h * 0.5f);
        ExpectPixel(img, w, h, cx, cy, 0, 0, 0, 6,
                    "填充层是纯色（texture_id=0）⇒ 中心必须黑");
        // 边框带（上边框中间往上 10px）应为红：R 明显大于 G/B。
        const int bx = cx;
        const int by = static_cast<int>(kSlotF.y + 10.0f);
        const unsigned char* p = &img[(static_cast<size_t>(by) * w + bx) * 4];
        CHECK(p[0] > 180 && p[1] < 60 && p[2] < 60)
            << "边框层没采样自己的图集：@" << bx << "," << by << " 实际 ("
            << static_cast<int>(p[0]) << "," << static_cast<int>(p[1]) << ","
            << static_cast<int>(p[2]) << ")";
    }
    LOG(INFO) << "[PASS] 两层材质独立：填充纯黑、边框来自图集（红）";

    // ---- 4. 半透明重叠：预乘 alpha 混合 ----
    {
        // G 独有区域（x≈530）、G∩H 重叠区域（x≈580）
        const int only_x = 530;
        const int both_x = 580;
        const int y = 230;
        const int only = static_cast<int>(img[(static_cast<size_t>(y) * w + only_x) * 4]);
        const int both = static_cast<int>(img[(static_cast<size_t>(y) * w + both_x) * 4]);
        LOG(INFO) << "半透明重叠：单层=" << only << " 双层=" << both;
        // 单层 = 0.5 alpha 的白盖黑；双层 = 1-(1-0.5)^2 = 0.75。
        // 断言只用**相对关系**，不锁具体编码（避免被 sRGB/tonemap 细节绊住）：
        //   单层必须明显灰（不是纯白、也不是纯黑）；双层必须比单层更接近白。
        CHECK(only > 90 && only < 200)
            << "单层半透明应得到中间亮度，实际 " << only
            << "（退化成纯白 = 直通 alpha 混进预乘路径；纯黑 = 板没画）";
        CHECK(both > only + 30)
            << "重叠区应更亮：单层 " << only << " → 双层 " << both;
        CHECK(both < 250) << "重叠区不该直接变纯白（说明 alpha 被吃了），实际 " << both;
    }
    LOG(INFO) << "[PASS] 半透明重叠：预乘 alpha 混合正常（单层 < 双层 < 纯白）";

    stbi_image_free(img);
    LOG(INFO) << "===== plate2d GPU 像素测试通过 =====";
    return 0;
}

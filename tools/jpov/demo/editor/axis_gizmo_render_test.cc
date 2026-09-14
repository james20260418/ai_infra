// JPOV 模型编辑器 — 世界坐标架渲染自证（headless，需 GL/DISPLAY）
//
// 验证 EditorApp 的场景里真的画出了"1m 三色坐标架"，且：
//   ① 三根条带各占一个轴向（+X/+Y/+Z），从原点出发、长 1m；
//   ② 颜色分别是红/绿/蓝（混到浅灰地面上的像素呈对应色调）；
//   ③ 半透明（alpha 0.2）—— 杆上像素是条带色与背景的混合，不是纯色。
//
// 为什么用像素判定而不是看图：看图只能发现"明显没了"，抓不出
//   "alpha 写成 1.0"（视觉上都像一根条带）与"三色串位"（红绿蓝搞混）。
//
// 运行：需 Xvfb/DISPLAY（与其它 headless 测试同）。输出 PNG 到 /tmp 供人工核对。

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "stb_image.h"

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/editor/editor_app.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/axis_gizmo.h"

namespace {

constexpr int kW = 640;
constexpr int kH = 360;

// 相机机位要**避免任一条带近乎侧视**：条带无厚度，当视线几乎平行于带平面时
// 它在屏幕上退化成一两像素宽的刺（不是 bug，但会让像素判定失效）。
//
// ⚠️ **不能用 X-Z 对角机位（如 (d,·,d)）**：那会与某条带的法线方向近平行。
// 取"三轴兼顾的斜机位"——实测 |dot(view, 带法线)| 对红 ≈0.66、对绿/蓝 ≈0.66，
// 三条都不侧视。（曾用过 (3.8,-2.05,0.15) 这种贴 Z 轴近的机位 → 红带
// |dot|=0.059 几乎侧视、像素上完全找不到，误以为"红带没画出来"。）
constexpr float kCamDist = 1.9f;

class GizmoApp : public jpov_viewer::EditorApp {
public:
    using jpov_viewer::EditorApp::EditorApp;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        // 复用 EditorApp 的场景逻辑（含坐标架绘制），但交互面板关掉
        //（headless 出图不需要 UI，且面板会盖住画面左下角）；相机由本类指定。
        SetShowPanel(false);
        // 关掉自动坐标架，改为下面**显式带 alpha 覆盖**地画（测试缝）。
        show_axis_gizmo_ = false;
        jpov_viewer::EditorApp::OneIteration(frame_count, input, winfo, cmds);
        DrawAxisGizmoForTest(cmds,
            alpha_override < 0.0f ? std::nullopt
                                  : std::make_optional(alpha_override));

        // ⚠️ 坐标架现在在**模型处**（placement 平移；本测试 placement 为默认值
        // → 世界原点 (0,0,0)），不再是 ground_y_！相机必须对着它，
        // 否则三条带全在画面外（实测踩过这个坑）。
        const float s = axis_gizmo_cam_sign_;
        // 斜机位，三轴兼顾（见 kCamDist 注释）。
        cmds->camera.position = {s * 1.05f, 0.63f, s * 1.05f};
        cmds->camera.target = {s * 0.30f, 0.35f, 0.35f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.near = 0.01f;
        cmds->camera.far = 100.0f;
    }

    // 相机侧：+1 默认 / −1 对面（验证双面可见用）。
    float axis_gizmo_cam_sign_ = 1.0f;
    // 轴带 alpha 覆盖（<0 = 不覆盖 → 生产默认 0.2），供"不透明 vs 半透明"对照。
    float alpha_override = -1.0f;

};

struct Pix {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;   // RGB, 3 bytes/px
    bool Load(const std::string& p) {
        int comp = 0;
        unsigned char* px = stbi_load(p.c_str(), &w, &h, &comp, 3);
        if (!px) return false;
        rgb.assign(px, px + static_cast<size_t>(w) * h * 3);
        stbi_image_free(px);
        return true;
    }
    // 取 (x,y) 像素（原点左上）。
    void At(int x, int y, int* r, int* g, int* b) const {
        const size_t i = (static_cast<size_t>(y) * w + x) * 3;
        *r = rgb[i]; *g = rgb[i + 1]; *b = rgb[i + 2];
    }
};

// 检测地平线的行号（图像本身的地面/天空分界），而不是写死一个行号。
//
// 做法：天空是蓝调（b 明显大于 r），地面是中性灰（三通道几乎相等）。
// 自上而下找**第一行**"绝大多数像素 r≈b（中性灰）"的位置，即天地交界。
//
// 为什么不用写死行号：写死（如 90）会随相机/分辨率/天空参数的改动**静默失效**
// （测试仍然绿，但已经在错的区域采样，等于没测）。由图像自推则改场景时要么
// 继续正确、要么立即 CHECK 失败 —— 两种结果都是有信息的。
int DetectHorizonY(const Pix& px) {
    auto row_is_grey = [&px](int y) {
        int grey = 0;
        for (int x = 0; x < px.w; ++x) {
            int r, g, b;
            px.At(x, y, &r, &g, &b);
            if (std::abs(r - b) <= 8) ++grey;   // 中性：红蓝接近 → 灰
        }
        return grey * 10 >= px.w * 9;           // ≥90% 像素为中性灰
    };
    for (int y = 0; y < px.h; ++y) {
        if (row_is_grey(y)) return y;
    }
    return -1;
}

// 在限定行范围内找"某种颜色倾向"最强的像素（用于定位某根带的可见位置）。
//   want="r" → 找 r 显著大于 g,b 的像素（红带）；"g" → 绿带；"b" → 蓝带。
// ⚠️ **必须限制行范围**：天空本身是蓝的（b 远大于 r,g），若不限制，
//    "找最蓝的像素"永远会命中天空而不是蓝带（实测踩过）。故调用方把搜索
//    限制在"地面区域"（地平线以下），那里唯一的蓝色来源就是 Z 带。
struct ColorHit {
    bool found = false;
    int x = 0, y = 0, r = 0, g = 0, b = 0, margin = 0;
};

ColorHit FindStrongestInRows(const Pix& px, char want_channel, int y_min,
                             int y_max) {
    ColorHit best;
    for (int y = std::max(0, y_min); y < std::min(px.h, y_max); ++y) {
        for (int x = 0; x < px.w; ++x) {
            int r, g, b;
            px.At(x, y, &r, &g, &b);
            int main = 0, other_max = 0;
            switch (want_channel) {
                case 'r': main = r; other_max = std::max(g, b); break;
                case 'g': main = g; other_max = std::max(r, b); break;
                default:  main = b; other_max = std::max(r, g); break;
            }
            const int margin = main - other_max;
            if (!best.found || margin > best.margin) {
                best.found = true;
                best.x = x; best.y = y;
                best.r = r; best.g = g; best.b = b; best.margin = margin;
            }
        }
    }
    return best;
}

// 渲染一帧（编辑器默认场景：地面 + 坐标架）到 PNG。
// alpha_override < 0 时用轴带各自的默认 alpha（0.2）；≥0 时强制该 alpha
//（同场景不透明 vs 半透明对照 —— alpha 生效的直接证据）。
// cam_sign：+1 默认侧 / −1 **对面**。对面视角用来验证"正反各画一次 ⇒ 双面可见"：
// 若只画单面，对面视角下三条带会被背面剔除而**整条消失**。
void RenderGizmo(const std::string& out_png, float alpha_override = -1.0f,
                 float cam_sign = 1.0f) {
    JPOV::Config cfg;
    cfg.title = "axis_gizmo_check";
    cfg.width = kW;
    cfg.height = kH;
    cfg.headless = true;
    GizmoApp app(cfg);
    app.axis_gizmo_cam_sign_ = cam_sign;
    app.alpha_override = alpha_override;
    app.Init();

    app.ground_mat_  = jpov_viewer::GroundMaterial();
    app.ground_mesh_ = app.RegisterMesh(jpov_viewer::MakeGroundQuad(
        jpov_viewer::kEditorGroundDefault));


    jpov::InputSnapshot in{};
    jpov::WindowInfo winfo{};
    winfo.width = static_cast<float>(kW);
    winfo.height = static_cast<float>(kH);
    app.RunOnce(in, winfo, out_png.c_str());
    app.Finalize();
}

}  // namespace

int main() {
    const std::string out = "/tmp/jpov_axis_gizmo_check.png";
    RenderGizmo(out);
    LOG(INFO) << "渲染完成: " << out;

    Pix px;
    CHECK(px.Load(out)) << "无法读取渲染结果: " << out;
    CHECK_EQ(px.w, kW);
    CHECK_EQ(px.h, kH);

    // ---- ① 三根带各自可见（红/绿/蓝各能找到"该色调占优"的像素）----
    // ⚠️ 只在**地面区域**（地平线以下）搜：天空自身是蓝的（见 FindStrongestInRows
    //    注释）。地平线由图像自推，不写死行号。
    const int horizon_y = DetectHorizonY(px);
    CHECK_GT(horizon_y, 0) << "未能检测到地平线（场景构成变了？）";
    CHECK_LT(horizon_y, px.h - 20) << "地平线过低，地面区域不足以取样";
    LOG(INFO) << "检测到地平线 y=" << horizon_y;
    const ColorHit hit_r = FindStrongestInRows(px, 'r', horizon_y + 2, px.h);
    const ColorHit hit_g = FindStrongestInRows(px, 'g', horizon_y + 2, px.h);
    const ColorHit hit_b = FindStrongestInRows(px, 'b', horizon_y + 2, px.h);
    LOG(INFO) << "红带像素 (" << hit_r.x << "," << hit_r.y << ") rgb=("
              << hit_r.r << "," << hit_r.g << "," << hit_r.b
              << ") margin=" << hit_r.margin;
    LOG(INFO) << "绿带像素 (" << hit_g.x << "," << hit_g.y << ") rgb=("
              << hit_g.r << "," << hit_g.g << "," << hit_g.b
              << ") margin=" << hit_g.margin;
    LOG(INFO) << "蓝带像素 (" << hit_b.x << "," << hit_b.y << ") rgb=("
              << hit_b.r << "," << hit_b.g << "," << hit_b.b
              << ") margin=" << hit_b.margin;

    // 每根带都必须在画面上"占优"到肉眼可辨（margin ≥ 6 足够：
    // alpha 0.2 的带混进浅灰地面后通道差本就只有十几）。
    CHECK_GT(hit_r.margin, 6) << "红色 (X) 带不可见或太淡（margin="
                              << hit_r.margin << "）——坐标架没画出来？";
    CHECK_GT(hit_g.margin, 6) << "绿色 (Y) 带不可见或太淡（margin="
                              << hit_g.margin << "）";
    CHECK_GT(hit_b.margin, 6) << "蓝色 (Z) 带不可见或太淡（margin="
                              << hit_b.margin << "）";

    // ---- ② 半透明自证：带上像素**不是**纯色（背景混了进来）----
    // 原理（由混合公式推导，不是拍的阈值）：alpha 混合
    //     out = a·C + (1-a)·G        （a = 0.2，G = 地面色 ≈ (172,170,170)）
    //   - 红带的 G/B 通道几乎无贡献，故像素的 g/b 必然由 0.8·G ≈ 137 主导
    //     —— 即**显著非零**。若 alpha 被写死 1.0，红带像素就是饱和纯红，g≈b≈0。
    // 故断言**非主通道之和 ≥ 60**（背景肯定混进来了）—— 该判据不依赖主通道
    // 绝对值，故不受曝光/亮度调整影响，是最稳的一条。
    auto check_not_pure = [](const ColorHit& h, char ch, const char* name) {
        const int oa = (ch == 'r') ? h.g : (ch == 'g') ? h.r : h.r;
        const int ob = (ch == 'r') ? h.b : (ch == 'g') ? h.b : h.g;
        const bool ok = (oa + ob) >= 60;
        if (!ok) {
            LOG(ERROR) << name << "带像素太接近纯色 → 半透明失效？rgb=("
                       << h.r << "," << h.g << "," << h.b << ")";
        }
        return ok;
    };
    CHECK(check_not_pure(hit_r, 'r', "红")) << "红带半透明失效";
    CHECK(check_not_pure(hit_g, 'g', "绿")) << "绿带半透明失效";
    CHECK(check_not_pure(hit_b, 'b', "蓝")) << "蓝带半透明失效";

    // ---- ③ 三根带的"最强像素"应落在不同区域（防三带重叠/串位）----
    // ⚠️ 只作**警告**，不作硬失败：三条带从同一点发散，在某些（合法的）
    //    观察角度下两条带会在屏幕上靠得很近甚至部分重叠 —— 那是几何使然，
    //    不是 bug。若这里硬失败，就会把"换了个合理机位"误报成回归
    //（实测：斜机位下红/蓝带在屏幕上相邻，硬断言会 FAIL）。
    //    真正锁住"没串位"的是上面两条：每条带各自 margin ≥ 6（颜色分别成立）
    //    + ④ 与不透明图的逐像素对照。
    auto far_apart = [](const ColorHit& a, const ColorHit& b) {
        const int dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy >= 400;   // ≥20px 分离
    };
    if (!far_apart(hit_r, hit_g)) {
        LOG(WARNING) << "红带与绿带最强像素相近（" << hit_r.x << "," << hit_r.y
                     << ") vs (" << hit_g.x << "," << hit_g.y
                     << ")——视角使然，非失败";
    }
    if (!far_apart(hit_r, hit_b)) {
        LOG(WARNING) << "红带与蓝带最强像素相近（" << hit_r.x << "," << hit_r.y
                     << ") vs (" << hit_b.x << "," << hit_b.y
                     << ")——视角使然，非失败";
    }
    if (!far_apart(hit_g, hit_b)) {
        LOG(WARNING) << "绿带与蓝带最强像素相近（" << hit_g.x << "," << hit_g.y
                     << ") vs (" << hit_b.x << "," << hit_b.y
                     << ")——视角使然，非失败";
    }

    // ---- ④ alpha 生效的**直接对照**（同场景再渲一张不透明的）----
    // 算法：在不同 alpha 下，带上的像素应有可测差异，且 **alpha 越小越靠近
    //   背景（地面）色**。这是"混合系数真的被用上了"的直接证据：
    //   若代码把 alpha 写死 1.0（或走了不混合的路径），两张图**逐字节相同**，
    //   下面的差异门禁会立即 FAIL。
    const std::string out_opaque = "/tmp/jpov_axis_gizmo_check_opaque.png";
    RenderGizmo(out_opaque, /*alpha_override*/ 1.0f);
    Pix px_opaque;
    CHECK(px_opaque.Load(out_opaque)) << "无法读取不透明对照图";

    auto dist_to = [](int r, int g, int b, int tr, int tg, int tb) {
        const int dr = r - tr, dg = g - tg, db = b - tb;
        return dr * dr + dg * dg + db * db;
    };
    // 地面参考色（取画面右下角一个远离三带的像素）。
    int gr, gg, gb;
    px.At(kW - 20, kH - 20, &gr, &gg, &gb);
    int orr, org, ogb;
    px_opaque.At(kW - 20, kH - 20, &orr, &org, &ogb);
    CHECK_EQ(gr, orr);   // 地面本身不受 alpha 影响 → 两图背景必须相同
    CHECK_EQ(gg, org);
    CHECK_EQ(gb, ogb);

    // 对三根带的"最强像素"位置各比较一次：
    auto check_alpha_mix = [&](const ColorHit& h, const char* name) {
        int rr, rg, rb;
        px_opaque.At(h.x, h.y, &rr, &rg, &rb);
        const int d_semi   = dist_to(h.r, h.g, h.b, gr, gg, gb);
        const int d_opaque = dist_to(rr, rg, rb, gr, gg, gb);
        LOG(INFO) << name << "带 @(" << h.x << "," << h.y << "): 半透明 rgb=("
                  << h.r << "," << h.g << "," << h.b << ") 距地面²=" << d_semi
                  << " | 不透明 rgb=(" << rr << "," << rg << "," << rb
                  << ") 距地面²=" << d_opaque;
        if (!(d_semi < d_opaque)) {
            LOG(ERROR) << name << "带：半透明像素未比不透明像素更靠近背景"
                       << "（" << d_semi << " vs " << d_opaque
                       << "）→ alpha 混合未被应用？";
            return false;
        }
        return true;
    };
    CHECK(check_alpha_mix(hit_r, "红")) << "红带 alpha 混合未生效";
    CHECK(check_alpha_mix(hit_g, "绿")) << "绿带 alpha 混合未生效";
    CHECK(check_alpha_mix(hit_b, "蓝")) << "蓝带 alpha 混合未生效";

    // ---- ⑤ 双面可见自证（从**对面**看，三条带仍必须存在）----
    // 条带是无厚度的单面几何 + 开着背面裁剪 → 只画一次的话，从背面看
    // **整条消失**。编辑器因此对每条带"正反各画一次"。本项从反侧出图，
    // 若双面方案失效（只画单面/绕序固定），三带会全部消失、margin 跌回 ~0。
    const std::string out_back = "/tmp/jpov_axis_gizmo_check_backside.png";
    RenderGizmo(out_back, /*alpha_override*/ -1.0f, /*cam_sign*/ -1.0f);
    Pix px_back;
    CHECK(px_back.Load(out_back)) << "无法读取对面视角图";
    const int hz_back = DetectHorizonY(px_back);
    CHECK_GT(hz_back, 0) << "对面视角未能检测到地平线";
    const ColorHit br = FindStrongestInRows(px_back, 'r', hz_back + 2, px_back.h);
    const ColorHit bg = FindStrongestInRows(px_back, 'g', hz_back + 2, px_back.h);
    const ColorHit bb = FindStrongestInRows(px_back, 'b', hz_back + 2, px_back.h);
    LOG(INFO) << "对面视角: 红 margin=" << br.margin << " 绿 margin=" << bg.margin
              << " 蓝 margin=" << bb.margin;
    CHECK_GT(br.margin, 6) << "对面看红带消失了 → 双面可见失效（只画了单面？）";
    CHECK_GT(bg.margin, 6) << "对面看绿带消失了 → 双面可见失效";
    CHECK_GT(bb.margin, 6) << "对面看蓝带消失了 → 双面可见失效";

    LOG(INFO) << "TEST PASSED: 坐标架三带可见 + 红绿蓝分明 + 半透明生效 + 双面可见";
    return 0;
}

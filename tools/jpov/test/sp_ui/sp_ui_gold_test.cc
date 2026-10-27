// JPOV sp_ui gold 测试 —— 纯 CPU 指令层（无窗口、无 GL）
//
// 目的：给 sp_ui 钉住「一旦破就会静默变丑」的那几件事。布局/指令全貌的逐条比对
// 由旧 ui 的 S7 测试负责（那份仍然是 interface:ui 的契约）；本测试只抓 sp_ui 独有的、
// 且已经在开发期真出过 bug 的机理：
//
//   1. **文字必须落在自己的控件上**（回归：全局缩放启用后，控件内部调公开 Text()
//      导致 box 被缩放两次 ⇒ 文字以 scale² 偏移、飞出按钮。scale=1 时完全看不出来，
//      所以必须用 scale=1.5 测。）
//   2. **缩放必须处处同步**：box / 字号 / 全部板材 pos+size 都要恰好乘同一个倍率。
//   3. **两层材质各自独立**：填充层与边框层的纹理 id、图集、帧率互不串台；
//      色样是「纯色填充 + 纹理边框」的特例。
//   4. **UV 锚到屏幕原点**：scroll 必须等于 −pos/material_px（否则同尺寸控件长得一样）。
//   5. **时间即数据**：所有板材的 time_seconds 恒等于调用方传入的秒数。
//   6. **FlipbookDesc / ResolveFlipbookFrame** 的 wrap 语义（含 t = N/fps 回到第 0 帧）。
//   7. 退化输入：零尺寸/越界不产出指令；圆角超限在 UI 层被 clamp 而不是崩。
//
// 全部断言都是「能区分对错」的：把上面对应的实现改回错误版本，测试必须 FAIL。

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/sp_ui_demo_panel.h"
#include "tools/jpov/interface/flipbook.h"
#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/sp_ui/sp_ui.h"

namespace {

using jpov::Color;
using jpov::Plate2DCommand;
using jpov::RenderCommandList;
using jpov::sp_ui::SpDemoState;
using jpov::sp_ui::SpRect;
using jpov::sp_ui::SpTheme;
using jpov::sp_ui::SpUi;

// 测试用的两个「纹理 id」—— 只需要非 0 且不同，CPU 侧不解析真纹理。
constexpr uint32_t kCoreTex = 101;
constexpr uint32_t kRimTex = 202;

// 主题：填充层 6fps / 边框层 3fps、不同纹理、固定密度。
// 这三个差异就是断言「两层没串台」的抓手。
SpTheme MakeTheme() {
    SpTheme t = SpTheme::Default(16.0f);
    t.plate_texture_id = kCoreTex;
    t.plate_material_px = 200.0f;
    t.plate_flipbook_fps = 6.0f;
    t.plate_tiles_x = 4;
    t.plate_tiles_y = 4;
    t.plate_border_texture_id = kRimTex;
    t.plate_border_material_px = 120.0f;
    t.plate_border_flipbook_fps = 3.0f;
    t.plate_border_tiles_x = 4;
    t.plate_border_tiles_y = 4;
    t.plate_border_bump = 1.0f;
    t.corner_radius_px = 6.0f;
    t.border_width_px = 3.0f;
    return t;
}

// RenderPanelOnce 里的 DrawSpUIDemoPanel 需要一个 RenderCommandList*（面板已改为
// 全部走 ui.Plate，但不接受 nullptr）。给一个静态占位，不入断言。
RenderCommandList& dummy_cmds() {
    static RenderCommandList dummy;
    dummy.Clear();
    return dummy;
}

// 跑一帧整台面板，返回生成的指令表。
//
// 输入全空、状态取默认、时间固定 —— gold 的可复现前提。
RenderCommandList RenderPanelOnce(float scale, float time_seconds) {
    SpUi ui;
    ui.SetUiScale(scale);
    SpDemoState st;
    st.showcase_core_tex = kCoreTex;
    st.showcase_rim_tex = kRimTex;

    jpov::InputSnapshot input{};
    SpTheme theme = MakeTheme();
    const float w = jpov::sp_ui::kSpUiDemoWidth * scale;
    const float h = jpov::sp_ui::kSpUiDemoHeight * scale;
    ui.Begin(input, theme, w, h, /*frame_dt_ms=*/0.0f, time_seconds);
    jpov::sp_ui::DrawSpUIDemoPanel(ui, &dummy_cmds(), st);
    ui.End();

    RenderCommandList cmds;
    ui.Emit(&cmds);
    return cmds;
}

// 点是否落在板材矩形内（含边界）。
bool PlateContains(const Plate2DCommand& p, const jpov::Vec2f& pt) {
    return pt.x() >= p.pos.x() && pt.x() <= p.pos.x() + p.size.x() &&
           pt.y() >= p.pos.y() && pt.y() <= p.pos.y() + p.size.y();
}

// 找包含 pt 的**最小**板材（数组下标）；找不到返回 -1。
// 「最小」是关键：整块背景板包含所有文字，只有排除它才有判别力。
int SmallestPlateContaining(const RenderCommandList& cmds,
                            const jpov::Vec2f& pt) {
    int best = -1;
    float best_area = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(cmds.plate2d.size()); ++i) {
        const Plate2DCommand& p = cmds.plate2d[i];
        if (!PlateContains(p, pt)) {
            continue;
        }
        const float area = p.size.x() * p.size.y();
        if (area < best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

// 找指定文本的指令下标；找不到返回 -1。
int FindText(const RenderCommandList& cmds, const std::string& s) {
    for (int i = 0; i < static_cast<int>(cmds.text2d.size()); ++i) {
        if (cmds.text2d[i].text == s) {
            return i;
        }
    }
    return -1;
}

// ---- 1. 文字必须落在自己的控件上 ----
//
// 回归用例：scale=1.5 时，Button 内部曾调公开的 Text()（box 已被缩过一次）⇒
// 文字位置 = scale² 倍偏移，飞出按钮。这里用「包含该文字的最小板材」判定。
void CheckLabelsSitOnTheirControls(float scale) {
    const RenderCommandList cmds = RenderPanelOnce(scale, 1.0f);
    CHECK_GT(cmds.plate2d.size(), 20u) << "板材数量异常少，面板没画全";

    // 按钮类标签：必须落在**小尺寸**板材里（不是整块背景板）。
    static const char* const kButtonLabels[] = {"重置", "应用"};
    for (const char* label : kButtonLabels) {
        const int ti = FindText(cmds, label);
        CHECK_GE(ti, 0) << "找不到按钮标签：" << label;
        const jpov::Vec2f pt = cmds.text2d[ti].pos;
        const int pi = SmallestPlateContaining(cmds, pt);
        CHECK_GE(pi, 0) << label << " 的文字不在任何板材内（scale=" << scale << "）";
        const Plate2DCommand& plate = cmds.plate2d[pi];
        const float area = plate.size.x() * plate.size.y();
        // 按钮 120x30（布局）→ 各 scale 下都远小于背景板（1280x720 起）。
        CHECK_LT(area, 0.25f * 1280.0f * 720.0f * scale * scale)
            << label << " 的文字落在背景板上而不是按钮上（scale=" << scale
            << "，最小板材面积 " << area << "）—— 典型的“box 被缩放两次”";
        LOG(INFO) << "[PASS] 标签「" << label << "」在按钮内（scale=" << scale << "）";
    }
}

// ---- 2. 缩放必须处处同步 ----
void CheckScaleIsUniform() {
    const RenderCommandList one = RenderPanelOnce(1.0f, 1.0f);
    const RenderCommandList big = RenderPanelOnce(1.5f, 1.0f);
    CHECK_EQ(one.plate2d.size(), big.plate2d.size())
        << "缩放改变了板材数量（说明某处用了未缩放的坐标做剔除）";
    CHECK_EQ(one.text2d.size(), big.text2d.size()) << "缩放改变了文本数量";

    for (size_t i = 0; i < one.plate2d.size(); ++i) {
        const Plate2DCommand& a = one.plate2d[i];
        const Plate2DCommand& b = big.plate2d[i];
        CHECK(std::fabs((b.pos.x()) - (a.pos.x() * 1.5f)) < (0.01f)) << "plate[" << i << "].pos.x";
        CHECK(std::fabs((b.pos.y()) - (a.pos.y() * 1.5f)) < (0.01f)) << "plate[" << i << "].pos.y";
        CHECK(std::fabs((b.size.x()) - (a.size.x() * 1.5f)) < (0.01f)) << "plate[" << i << "].size.x";
        CHECK(std::fabs((b.size.y()) - (a.size.y() * 1.5f)) < (0.01f)) << "plate[" << i << "].size.y";
        CHECK(std::fabs((b.radius) - (a.radius * 1.5f)) < (0.02f)) << "plate[" << i << "].radius";
        CHECK(std::fabs((b.border_width) - (a.border_width * 1.5f)) < (0.02f))
            << "plate[" << i << "].border_width";
    }
    for (size_t i = 0; i < one.text2d.size(); ++i) {
        CHECK(std::fabs((big.text2d[i].pos.x()) - (one.text2d[i].pos.x() * 1.5f)) < (0.01f))
            << "text[" << i << "].pos.x";
        CHECK(std::fabs((big.text2d[i].pos.y()) - (one.text2d[i].pos.y() * 1.5f)) < (0.01f))
            << "text[" << i << "].pos.y";
        CHECK(std::fabs((big.text2d[i].font_size) - (one.text2d[i].font_size * 1.5f)) < (0.02f))
            << "text[" << i << "].font_size";
    }
    LOG(INFO) << "[PASS] scale=1.5 时板材/文本的位置、尺寸、字号、圆角、线宽同步放大";
}

// ---- 3. 两层材质各自独立 + 色样特例 ----
//
// 面板里有两类板材：主题驱动的控件底板（fill=kCoreTex@6fps / border=kRimTex@3fps）
// 与「多纹理板材」展示区（两层各自另有图集与帧率）。所以不能对**每块**板都要求
// 同一组值 —— 那样会把展示区误判成串台。做法：
//   ① 定向检查一块已知控件（「重置」按钮）的两层取值；
//   ② 统计「主题形态」板材数量必须占多数（串台时这个数会直接掉到 0）；
//   ③ 色样形态（纯色填充 + 纹理边框）必须存在。
void CheckTwoLayersAreIndependent() {
    const RenderCommandList cmds = RenderPanelOnce(1.0f, 1.0f);

    // ① 定向：「重置」按钮所在的最小板材。
    const int ti = FindText(cmds, "重置");
    CHECK_GE(ti, 0);
    const int bi = SmallestPlateContaining(cmds, cmds.text2d[ti].pos);
    CHECK_GE(bi, 0);
    const Plate2DCommand& btn = cmds.plate2d[bi];
    CHECK_EQ(btn.fill.texture_id, kCoreTex) << "按钮填充层没用 theme.plate_texture_id";
    CHECK(std::fabs(btn.fill.flipbook.fps - 6.0f) < 1e-4f)
        << "按钮填充层帧率没跟随 theme.plate_flipbook_fps（实际 "
        << btn.fill.flipbook.fps << "）";
    CHECK_EQ(btn.fill.flipbook.tiles_x, 4);
    CHECK_EQ(btn.border.texture_id, kRimTex)
        << "按钮边框层没用 theme.plate_border_texture_id";
    CHECK(std::fabs(btn.border.flipbook.fps - 3.0f) < 1e-4f)
        << "按钮边框层帧率没跟随 theme.plate_border_flipbook_fps（实际 "
        << btn.border.flipbook.fps << "）";
    CHECK_EQ(btn.border.flipbook.tiles_x, 4);
    // 两层密度必须不同（主题里 fill=200px/格、border=120px/格）。
    CHECK(std::fabs(btn.fill.tiling.x() - btn.border.tiling.x()) > 1e-3f)
        << "填充层与边框层密度一样 —— 两层的 material_px 串台了";

    // ② 统计主题形态板材；顺带确认没有「填充层拿了边框图集」的反向串台。
    int theme_shaped = 0;
    int swapped = 0;
    int solid_fill = 0;
    for (const Plate2DCommand& p : cmds.plate2d) {
        if (p.fill.texture_id == kCoreTex &&
            std::fabs(p.fill.flipbook.fps - 6.0f) < 1e-4f &&
            p.border.texture_id == kRimTex &&
            std::fabs(p.border.flipbook.fps - 3.0f) < 1e-4f) {
            ++theme_shaped;
        }
        if (p.fill.texture_id == kRimTex) {
            ++swapped;
        }
        if (p.fill.texture_id == 0u && p.border.texture_id != 0u) {
            ++solid_fill;
        }
    }
    CHECK_EQ(swapped, 0) << "填充层拿到了边框图集 —— 两层材质串台";
    CHECK_GT(theme_shaped, 20)
        << "主题形态板材只有 " << theme_shaped << " 块（应远多于 20）";
    CHECK_GE(solid_fill, 1)
        << "找不到「纯色填充 + 纹理边框」的板材（色样应为这种形态）";
    LOG(INFO) << "[PASS] 两层材质独立：主题形态 " << theme_shaped
              << " 块，纯色填充+纹理边框 " << solid_fill << " 块，无串台";
}

// ---- 4. UV 锚到屏幕原点 ----
void CheckScreenAnchoredUv() {
    const RenderCommandList cmds = RenderPanelOnce(1.0f, 1.0f);
    int checked = 0;
    for (const Plate2DCommand& p : cmds.plate2d) {
        if (p.fill.texture_id == 0u || p.fill.tiling.x() <= 0.0f) {
            continue;
        }
        // tiling = size/material_px、scroll = -pos/material_px ⇒
        // scroll = -pos / (size/tiling) = -pos * tiling / size
        const float expect_sx = -p.pos.x() * p.fill.tiling.x() / p.size.x();
        const float expect_sy = -p.pos.y() * p.fill.tiling.y() / p.size.y();
        CHECK(std::fabs((p.fill.scroll.x()) - (expect_sx)) < (0.01f))
            << "填充层 UV 没锚到屏幕原点（x）：scroll=" << p.fill.scroll.x()
            << " 期望 " << expect_sx;
        CHECK(std::fabs((p.fill.scroll.y()) - (expect_sy)) < (0.01f))
            << "填充层 UV 没锚到屏幕原点（y）";
        ++checked;
    }
    CHECK_GT(checked, 10) << "没有足够的有材质板材可供锚定检查";
    LOG(INFO) << "[PASS] 屏幕锚定 UV：" << checked << " 块板材的 scroll = −pos/material_px";
}

// ---- 5. 时间即数据 ----
void CheckTimeIsData() {
    for (float t : {0.0f, 1.25f, 7.5f}) {
        const RenderCommandList cmds = RenderPanelOnce(1.0f, t);
        for (const Plate2DCommand& p : cmds.plate2d) {
            CHECK(std::fabs((p.time_seconds) - (t)) < (1e-6f))
                << "板材没把调用方给的 time_seconds 原样带下去";
        }
    }
    // 时间不同 ⇒ 帧号不同（对比 CPU 侧解析，确认 dt 真的进了命令）。
    float f0 = 0.0f;
    float f1 = 0.0f;
    const jpov::FlipbookDesc fb{4, 4, 6.0f};
    CHECK(jpov::ResolveFlipbookFrame(fb, 0.0f, &f0));
    CHECK(jpov::ResolveFlipbookFrame(fb, 0.3f, &f1));
    CHECK(std::fabs((f1) - (1.8f)) < (1e-4f)) << "0.3s × 6fps 应为 1.8 帧（分数帧号）";
    CHECK(std::fabs(f1 - f0) > 1e-3f) << "不同时间给出了同一帧号";
    LOG(INFO) << "[PASS] time_seconds 原样进命令，且分数帧号随之间变化";
}

// ---- 6. FlipbookDesc / ResolveFlipbookFrame 的 wrap 语义 ----
void CheckFlipbookWrap() {
    const jpov::FlipbookDesc fb{4, 4, 6.0f};   // 16 帧 / 6fps ⇒ 一圈 16/6 秒
    CHECK_EQ(fb.frame_count(), 16);
    CHECK(fb.IsAnimated());

    float frame = -1.0f;
    // fps=0 = 静态（整张图一帧）：返回 false、帧号归零。
    const jpov::FlipbookDesc still{4, 4, 0.0f};
    CHECK(!jpov::ResolveFlipbookFrame(still, 3.0f, &frame));
    CHECK(std::fabs((frame) - (0.0f)) < (1e-6f));

    // 一圈回到起点：t = 16/6 与 t = 0 必须给出同一帧号。
    float a = 0.0f;
    float b = 0.0f;
    CHECK(jpov::ResolveFlipbookFrame(fb, 0.0f, &a));
    CHECK(jpov::ResolveFlipbookFrame(fb, 16.0f / 6.0f, &b));
    CHECK(std::fabs((b) - (a)) < (1e-3f)) << "整圈之后帧号没回到起点（循环会跳）";

    // 帧号恒在 [0, frame_count)。
    for (float t = -5.0f; t <= 5.0f; t += 0.13f) {
        CHECK(jpov::ResolveFlipbookFrame(fb, t, &frame));
        CHECK_GE(frame, 0.0f) << "t=" << t;
        CHECK_LT(frame, 16.0f) << "t=" << t;
    }
    // 负时间也要落进区间（调用方拿它做倒放）。
    CHECK(jpov::ResolveFlipbookFrame(fb, -0.5f, &frame));
    CHECK_GE(frame, 0.0f);
    CHECK_LT(frame, 16.0f);
    LOG(INFO) << "[PASS] flipbook 帧号：静态归零 / 整圈闭合 / 恒在 [0,N) / 负时间可用";
}

// ---- 7. 多纹理板材（MultiPlate）----
void CheckMultiPlate() {
    SpUi ui;
    jpov::InputSnapshot input{};
    ui.Begin(input, MakeTheme(), 1280.0f, 720.0f, 0.0f, 2.0f);

    const SpRect box{{100.0f, 100.0f}, {300.0f, 120.0f}};
    SpUi::SpPlateSkin skin;
    skin.fill = ui.MakeLayerFor(box, kCoreTex, 160.0f, {4, 4, 5.0f});
    skin.border = ui.MakeLayerFor(box, kRimTex, 80.0f, {4, 4, 9.0f});
    skin.fill_tint = {0.3f, 0.3f, 0.3f, 1.0f};
    skin.border_tint = {0.9f, 0.9f, 0.9f, 1.0f};
    skin.radius = 8.0f;
    skin.border_width = 6.0f;
    skin.border_bump = 1.0f;
    ui.MultiPlate(box, skin);
    ui.End();

    RenderCommandList cmds;
    ui.Emit(&cmds);
    CHECK_EQ(cmds.plate2d.size(), 1u) << "MultiPlate 应恰好产出一条板材指令";
    const Plate2DCommand& p = cmds.plate2d[0];
    CHECK_EQ(p.fill.texture_id, kCoreTex);
    CHECK_EQ(p.border.texture_id, kRimTex);
    // 两层图集/帧率/密度都必须各按传入值，不许互相污染。
    CHECK(std::fabs((p.fill.flipbook.fps) - (5.0f)) < (1e-4f));
    CHECK(std::fabs((p.border.flipbook.fps) - (9.0f)) < (1e-4f));
    CHECK(std::fabs((p.fill.tiling.x()) - (300.0f / 160.0f)) < (1e-3f)) << "核心密度算错";
    CHECK(std::fabs((p.border.tiling.x()) - (300.0f / 80.0f)) < (1e-3f)) << "边界密度算错";
    CHECK(std::fabs((p.time_seconds) - (2.0f)) < (1e-6f)) << "时间没带下去";
    CHECK(std::fabs((p.border_width) - (6.0f)) < (1e-4f));
    LOG(INFO) << "[PASS] MultiPlate：两层图集/帧率/密度互不污染";
}

// ---- 8. 退化输入 ----
void CheckDegenerateInputs() {
    SpUi ui;
    jpov::InputSnapshot input{};
    ui.Begin(input, MakeTheme(), 1280.0f, 720.0f, 0.0f, 0.0f);
    // 零尺寸 / 负尺寸：不产出指令、不崩。
    ui.Plate(SpRect{{10.0f, 10.0f}, {0.0f, 0.0f}}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 4.0f);
    ui.Plate(SpRect{{10.0f, 10.0f}, {-5.0f, 20.0f}}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 4.0f);
    // 完全在视口外：越界剔除。
    ui.Plate(SpRect{{5000.0f, 5000.0f}, {100.0f, 100.0f}}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 4.0f);
    // 圆角远大于短边：UI 层 clamp（渲染层是 CHECK 崩，不能把越界值传下去）。
    ui.Plate(SpRect{{100.0f, 100.0f}, {100.0f, 40.0f}}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 999.0f);
    ui.End();

    RenderCommandList cmds;
    ui.Emit(&cmds);
    CHECK_EQ(cmds.plate2d.size(), 1u) << "只有最后一块合法的板材应产出指令";
    const Plate2DCommand& p = cmds.plate2d[0];
    CHECK(std::fabs((p.radius) - (20.0f)) < (1e-4f)) << "圆角应被 clamp 到短边一半（20）";
    LOG(INFO) << "[PASS] 退化输入：零/负尺寸与越界被剔除，圆角被 clamp";
}

}  // namespace

int main() {
    CheckLabelsSitOnTheirControls(1.0f);
    CheckLabelsSitOnTheirControls(1.5f);
    CheckScaleIsUniform();
    CheckTwoLayersAreIndependent();
    CheckScreenAnchoredUv();
    CheckTimeIsData();
    CheckFlipbookWrap();
    CheckMultiPlate();
    CheckDegenerateInputs();
    LOG(INFO) << "===== sp_ui gold 全部通过 =====";
    return 0;
}

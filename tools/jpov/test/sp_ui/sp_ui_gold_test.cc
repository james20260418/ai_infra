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
using jpov::sp_ui::SpButtonSkin;
using jpov::sp_ui::SpRect;
using jpov::sp_ui::SpTheme;
using jpov::PlateLayer;
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

// ---- 6. FlipbookDesc / ResolveFlipbookFrame ----
//
// 两条独立语义，**不能合并判断**（早期版本合并过：4x4 图集配 fps=0 会整张显示，
// 屏幕上出现 16 个缩小的自己 —— 那是真 bug）：
//   裁 cell  ←→ tiles 是否 > 1
//   会不会动 ←→ fps 是否 > 0
void CheckFlipbookWrap() {
    const jpov::FlipbookDesc fb{4, 4, 6.0f};   // 16 帧 / 6fps ⇒ 一圈 16/6 秒
    CHECK_EQ(fb.frame_count(), 16);
    CHECK(fb.IsAtlas());
    CHECK(fb.IsAnimated());

    float frame = -1.0f;

    // ① 图集 + fps=0：**仍然要裁 cell**，只是静止在第 0 帧。
    const jpov::FlipbookDesc still_atlas{4, 4, 0.0f};
    CHECK(still_atlas.IsAtlas()) << "4x4 是图集（与帧率无关）";
    CHECK(!still_atlas.IsAnimated()) << "fps=0 时不动";
    CHECK(jpov::ResolveFlipbookFrame(still_atlas, 3.0f, &frame))
        << "图集必须裁 cell —— 不裁就会把整张 4x4 当一幅图显示";
    CHECK(std::fabs(frame) < 1e-6f) << "静止图集应固定在第 0 帧";

    // ② 单张图（tiles 1x1）：不裁 cell，帧号恒 0（fps 有意义与否都不裁）。
    for (float fps : {0.0f, 6.0f}) {
        const jpov::FlipbookDesc single{1, 1, fps};
        CHECK(!single.IsAtlas());
        CHECK(!jpov::ResolveFlipbookFrame(single, 3.0f, &frame))
            << "1x1 不是图集，不该裁 cell";
        CHECK(std::fabs(frame) < 1e-6f);
    }

    // ③ 一圈回到起点：t = 16/6 与 t = 0 必须给出同一帧号。
    float a = 0.0f;
    float b = 0.0f;
    CHECK(jpov::ResolveFlipbookFrame(fb, 0.0f, &a));
    CHECK(jpov::ResolveFlipbookFrame(fb, 16.0f / 6.0f, &b));
    CHECK(std::fabs(b - a) < 1e-3f) << "整圈之后帧号没回到起点（循环会跳）";

    // ④ 帧号恒在 [0, frame_count)，负时间也落在区间内（调用方拿它做倒放）。
    for (float t = -5.0f; t <= 5.0f; t += 0.13f) {
        CHECK(jpov::ResolveFlipbookFrame(fb, t, &frame));
        CHECK_GE(frame, 0.0f) << "t=" << t;
        CHECK_LT(frame, 16.0f) << "t=" << t;
    }
    CHECK(jpov::ResolveFlipbookFrame(fb, -0.5f, &frame));
    CHECK_GE(frame, 0.0f);
    CHECK_LT(frame, 16.0f);
    LOG(INFO) << "[PASS] flipbook：裁 cell 与动画分离 / 整圈闭合 / 恒在 [0,N) / 负时间可用";
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

// ---- 7b. MakeLayerFor 的密度与缩放无关 ----
//
// material_px 是**布局单位**，实现里是 dev.size / (material_px × scale)，
// 两边的 scale 应当约掉 ⇒ 同一块布局 box、不同 ui_scale，tiling 必须**相同**。
// 写错（漏乘或重复乘）会让 1.5 倍面板上的纹理密度突然变 2.25 倍 / 0.44 倍。
void CheckLayerForScaleInvariance() {
    const SpRect box{{120.0f, 60.0f}, {300.0f, 120.0f}};
    float tiling[2] = {0.0f, 0.0f};
    float scroll[2] = {0.0f, 0.0f};
    const float scales[2] = {1.0f, 1.5f};
    for (int i = 0; i < 2; ++i) {
        SpUi ui;
        ui.SetUiScale(scales[i]);
        jpov::InputSnapshot input{};
        ui.Begin(input, MakeTheme(), 1280.0f * scales[i], 720.0f * scales[i],
                 0.0f, 0.0f);
        const PlateLayer l = ui.MakeLayerFor(box, kCoreTex, /*material_px=*/150.0f,
                                             {4, 4, 6.0f});
        tiling[i] = l.tiling.x();
        scroll[i] = l.scroll.x();
        ui.End();
    }
    CHECK(std::fabs(tiling[0] - tiling[1]) < 1e-4f)
        << "密度随缩放变了（应约掉）：scale1.0 tiling=" << tiling[0]
        << "，scale1.5 tiling=" << tiling[1];
    CHECK(std::fabs(tiling[0] - 300.0f / 150.0f) < 1e-4f)
        << "密度算错：期望 2.0，实际 " << tiling[0];
    // 屏幕锚定同理：scroll = −pos/material_px，与缩放无关。
    CHECK(std::fabs(scroll[0] - (-120.0f / 150.0f)) < 1e-4f);
    CHECK(std::fabs(scroll[0] - scroll[1]) < 1e-4f)
        << "屏幕锚定的 scroll 随缩放变了";
    LOG(INFO) << "[PASS] MakeLayerFor：密度/锚定与 ui_scale 无关";
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

// ---- 9. 交互状态机（跨帧）----
//
// sp_ui 是从 ui.cc 复制来的：控件动线一致，但**这半边一条测试都没有**。
// 这里用伪造快照跑三件最容易在复制/重构中悄悄失效的事：
//   a) 点击语义：释放位置在框内才算点击
//   b) Combo 弹层独占：弹层覆盖区域里的点击不能穿透到下面的控件
//      （sp_ui 的 PushPopupPlate 改成设备坐标 + 两层材质，最容易在这里出错）
//   c) 一次 drag 语义：按下发生在滑条内之后，鼠标飘出竖直范围仍继续写值

// 一个最小场景：Combo 在上、Button 故意放在「弹层展开后会盖住」的位置。
struct MiniScene {
    SpUi ui;
    SpTheme theme = MakeTheme();
    RenderCommandList cmds;
    int combo_sel = 0;
    float slider = 0.0f;
    bool button_hit = false;
    bool slider_changed = false;

    static const std::vector<const char*>& Items() {
        static const std::vector<const char*> kItems = {"aa", "bb", "cc"};
        return kItems;
    }
    static SpRect ComboRect() { return {{10.0f, 40.0f}, {200.0f, 26.0f}}; }
    static SpRect ButtonRect() { return {{10.0f, 90.0f}, {200.0f, 26.0f}}; }
    static SpRect SliderRect() { return {{10.0f, 150.0f}, {300.0f, 26.0f}}; }

    void Frame(const jpov::InputSnapshot& in, float dt_ms = 0.0f) {
        cmds.Clear();
        ui.Begin(in, theme, 400.0f, 400.0f, dt_ms, 0.0f);
        button_hit = ui.Button("button", ButtonRect());
        ui.Combo("combo", &combo_sel, Items(), ComboRect());
        slider_changed = ui.SliderFloat("slider", &slider, SliderRect(), 0.0f, 100.0f);
        ui.End();
        ui.Emit(&cmds);
    }
};

// 伪造一次「按下后松开」：Hold 帧 + Click 帧（Click 要带坐标）。
jpov::InputSnapshot MakeHold(float x, float y) {
    jpov::InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    in.left.raw = -2;
    return in;
}
jpov::InputSnapshot MakeClick(float x, float y) {
    jpov::InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    in.left.raw = 1;
    in.left_clicks[0] = {x, y, 0.5f};
    return in;
}
jpov::InputSnapshot MakeDrag(float x, float y) {
    jpov::InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    in.left.raw = -1;
    return in;
}

void CheckInteractionStateMachine() {
    // ---- a) 点击：释放点落在按钮内才算点中 ----
    {
        MiniScene sc;
        sc.Frame(MakeHold(100.0f, 103.0f));
        sc.Frame(MakeClick(100.0f, 103.0f));
        CHECK(sc.button_hit) << "在按钮内按下并松开应返回 true";
        // 在按钮内按下、飘到框外松开 ⇒ 不算点击。
        MiniScene sc2;
        sc2.Frame(MakeHold(100.0f, 103.0f));
        sc2.Frame(MakeClick(100.0f, 300.0f));
        CHECK(!sc2.button_hit) << "在框外松开不应算作点击";
        LOG(INFO) << "[PASS] 点击语义：释放点必须落在按钮内";
    }

    // ---- b) Combo 弹层独占（点击不能穿透到下面的按钮）----
    {
        MiniScene sc;
        // 先确认（无弹层时）点按钮是能触发的。
        sc.Frame(MakeHold(100.0f, 103.0f));
        sc.Frame(MakeClick(100.0f, 103.0f));
        CHECK(sc.button_hit) << "基线：无弹层时按钮应可点";

        // 点开 Combo（它在上方，弹层向下展开、盖住按钮）。
        sc.Frame(MakeHold(100.0f, 53.0f));
        sc.Frame(MakeClick(100.0f, 53.0f));
        // 现在弹层已展开（本帧画出来，下一帧起独占其覆盖区域）。
        sc.Frame(jpov::InputSnapshot{});
        // 在按钮位置点一下 —— 该位置已被弹层盖住，必须**不**触发按钮。
        sc.Frame(MakeHold(100.0f, 103.0f));
        sc.Frame(MakeClick(100.0f, 103.0f));
        CHECK(!sc.button_hit)
            << "点在被已展开下拉盖住的位置，按钮不该被触发（弹层独占失效）";
        LOG(INFO) << "[PASS] Combo 弹层独占：被盖住的控件不接收点击";
    }

    // ---- c) 一次 drag 语义：按下在框内，之后飘出竖直范围仍继续写值 ----
    {
        MiniScene sc;
        sc.slider = 0.0f;
        // 在滑条内按下（Hold 即可开始 drag）
        sc.Frame(MakeHold(160.0f, 163.0f));
        const float after_press = sc.slider;
        // 拖到很右边、且 y 远在 box 之外
        sc.Frame(MakeDrag(300.0f, 400.0f));
        CHECK(sc.slider_changed)
            << "已经开始的 drag，鼠标飘出 box 竖直范围仍应继续写值";
        CHECK(sc.slider > after_press)
            << "往右拖应把值往右推：press=" << after_press
            << " → drag=" << sc.slider;
        LOG(INFO) << "[PASS] 一次 drag 语义：飘出竖直范围继续写值（"
                  << after_press << " → " << sc.slider << "）";
    }
}

// ---- 10. 翻书按钮：hover_ratio 渐变（跨帧状态机）----
//
// 要钉住的四件事（改错任意一条都能被下面的断言抓住）：
//   a) 增强按钮的填充层用的是**皮肤自己的图集/图集网格/帧率**，边框层仍然是主题的；
//   b) tint 按 dt/delay 线性爬升，**不是**一悬停就满；
//   c) 按下那一帧直接用 pressed_tint（不看 ratio），且 ratio 在后台继续衰减；
//     松开当帧从**当前** ratio 继续爬（不是先归零）；
//   d) 离开后衰减回 normal_tint；delay=0 / dt=0 两个退化分支都是「立即切」。
void CheckButtonHoverRatio() {
    const SpRect box{{10.0f, 10.0f}, {200.0f, 40.0f}};
    const float cx = box.pos.x() + box.size.x() * 0.5f;
    const float cy = box.pos.y() + box.size.y() * 0.5f;
    const float kDtMs = 100.0f;   // 每帧 0.1s；delay 0.4s ⇒ 每帧 +0.25

    SpButtonSkin skin;
    skin.texture_id = kCoreTex;   // 非 0：填充层必须换成这个图集
    skin.tiles_x = 4;
    skin.tiles_y = 4;
    skin.fps = 8.0f;
    skin.material_px = 100.0f;
    skin.normal_tint = {0.2f, 0.2f, 0.2f, 1.0f};
    skin.hover_tint = {1.0f, 0.6f, 0.2f, 1.0f};
    skin.pressed_tint = {0.05f, 0.04f, 0.03f, 1.0f};
    skin.normal_to_hover_delay = 0.4f;

    SpUi ui;
    SpTheme theme = MakeTheme();

    // 跑一帧，返回那颗按钮的板材指令（面板上只有它一个控件）。
    auto Frame = [&](const jpov::InputSnapshot& in, float dt_ms) {
        RenderCommandList cmds;
        ui.Begin(in, theme, 400.0f, 400.0f, dt_ms, /*time_seconds=*/0.0f);
        ui.Button("activate", box, skin);
        ui.End();
        ui.Emit(&cmds);
        CHECK_EQ(cmds.plate2d.size(), 1u) << "按钮应恰好产出一条板材指令";
        return cmds.plate2d[0];
    };
    auto Near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    auto CheckTint = [&](const Plate2DCommand& p, const Color& want,
                         const char* what) {
        CHECK(Near(p.fill_color.r, want.r) && Near(p.fill_color.g, want.g) &&
              Near(p.fill_color.b, want.b))
            << what << "：fill_color=(" << p.fill_color.r << ","
            << p.fill_color.g << "," << p.fill_color.b << ")，期望 ("
            << want.r << "," << want.g << "," << want.b << ")";
    };

    // a) 常态：填充层用皮肤图集，边框层仍是主题的。
    {
        jpov::InputSnapshot idle{};
        idle.mouse_x = 350.0f;   // 鼠标在按钮外
        idle.mouse_y = 350.0f;
        const Plate2DCommand p = Frame(idle, kDtMs);
        CHECK_EQ(p.fill.texture_id, kCoreTex) << "填充层应换成按钮皮肤的图集";
        CHECK_EQ(p.fill.flipbook.tiles_x, 4) << "填充层应用皮肤的图集网格";
        CHECK_EQ(p.fill.flipbook.tiles_y, 4) << "填充层应用皮肤的图集网格";
        CHECK(std::fabs(p.fill.flipbook.fps - 8.0f) < 1e-4f)
            << "填充层应用皮肤的翻书帧率";
        CHECK_EQ(p.border.texture_id, kRimTex)
            << "边框层不该被按钮皮肤改写（它仍然来自主题）";
        CheckTint(p, skin.normal_tint, "常态应为 normal_tint");
        LOG(INFO) << "[PASS] 增强按钮：填充层=皮肤图集，边框层=主题图集";
    }

    // b) 悬停 1 帧 = 0.25 比例（线性爬升，不是一跳到底）。
    jpov::InputSnapshot hover{};
    hover.mouse_x = cx;
    hover.mouse_y = cy;
    {
        const Plate2DCommand p = Frame(hover, kDtMs);
        CheckTint(p, Color{0.2f + 0.8f * 0.25f, 0.2f + 0.4f * 0.25f, 0.2f, 1.0f},
                  "悬停 1 帧（delay=0.4s, dt=0.1s）应为 25% 的 hover_tint");
        LOG(INFO) << "[PASS] hover_ratio 第 1 帧 = 0.25（线性爬升）";
    }
    // 再 3 帧 ⇒ 满（0.25 × 4 = 1.0）。
    {
        Plate2DCommand p{};
        for (int i = 0; i < 3; ++i) {
            p = Frame(hover, kDtMs);
        }
        CheckTint(p, skin.hover_tint, "悬停满 delay 后应为 hover_tint");
        // 再悬停不该越界（clamp 到 1）。
        const Plate2DCommand q = Frame(hover, kDtMs * 10.0f);
        CheckTint(q, skin.hover_tint, "ratio 必须 clamp 在 1，不会越界发白");
        LOG(INFO) << "[PASS] hover_ratio 在 delay 处到 1.0 并 clamp";
    }

    // c) 按下：直接用 pressed_tint；ratio 在后台衰减。
    {
        jpov::InputSnapshot press = hover;
        press.left.raw = -2;   // Hold 在按钮内
        const Plate2DCommand p = Frame(press, kDtMs);
        CheckTint(p, skin.pressed_tint, "按下那一帧应直接用 pressed_tint");
        Frame(press, kDtMs);   // 再按住一帧 ⇒ ratio 1.0 → 0.5
        // 松开（Click，鼠标仍在按钮上）：从当前 ratio 0.5 继续爬 ⇒ 0.75。
        jpov::InputSnapshot release = hover;
        release.left.raw = 1;
        release.left_clicks[0] = {cx, cy, 0.5f};
        const Plate2DCommand r = Frame(release, kDtMs);
        CheckTint(r, Color{0.2f + 0.8f * 0.75f, 0.2f + 0.4f * 0.75f, 0.2f, 1.0f},
                  "松开当帧应从当前 ratio 继续爬（不是跳回 normal）");
        LOG(INFO) << "[PASS] 按下=pressed_tint；松开后 ratio 从当前值续爬";
    }

    // d) 离开：衰减回 normal（每帧 −0.25，4 帧归零）。
    {
        jpov::InputSnapshot leave{};
        leave.mouse_x = 350.0f;
        leave.mouse_y = 350.0f;
        Plate2DCommand p{};
        for (int i = 0; i < 8; ++i) {   // 多跑几帧：确保不是靠「刚好 4 帧」
            p = Frame(leave, kDtMs);
        }
        CheckTint(p, skin.normal_tint, "离开后应衰减回 normal_tint");
        LOG(INFO) << "[PASS] hover_ratio 离开后衰减回 0";
    }

    // e) 两个退化分支：delay=0、dt=0 ⇒ 都是「立即切」。
    {
        SpUi ui2;
        SpButtonSkin instant = skin;
        instant.normal_to_hover_delay = 0.0f;
        auto Frame2 = [&](const jpov::InputSnapshot& in, float dt_ms,
                          const SpButtonSkin& sk) {
            RenderCommandList cmds;
            ui2.Begin(in, theme, 400.0f, 400.0f, dt_ms, 0.0f);
            ui2.Button("activate", box, sk);
            ui2.End();
            ui2.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 1u);
            return cmds.plate2d[0];
        };
        CheckTint(Frame2(hover, kDtMs, instant), skin.hover_tint,
                  "delay=0 应在一帧内直达 hover_tint");
        // 换一个全新 SpUi（不共享跨帧状态），用 dt=0 + 正常 delay。
        SpUi ui3;
        RenderCommandList cmds;
        ui3.Begin(hover, theme, 400.0f, 400.0f, /*frame_dt_ms=*/0.0f, 0.0f);
        ui3.Button("activate", box, skin);
        ui3.End();
        ui3.Emit(&cmds);
        CHECK_EQ(cmds.plate2d.size(), 1u);
        CheckTint(cmds.plate2d[0], skin.hover_tint,
                  "dt=0（无时钟）时应直接切到目标值");
        LOG(INFO) << "[PASS] delay=0 / dt=0 都退化为一帧直达";
    }

    // f) kFlipbook 模式：整块按钮 = 图集。与 kPlate 的差别必须体现在指令上：
    //    没有边框带（border_width=0）、边框层不采材质、圆角跟主题走；
    //    而 tint 渐变的语义与 kPlate 完全一致（同一套 hover_ratio）。
    {
        SpUi uif;
        SpButtonSkin baked = skin;
        baked.style = SpButtonSkin::Style::kFlipbook;
        baked.material_px = 0.0f;   // 整键皮不平铺：一格图铺满整颗按钮
        auto FrameF = [&](const jpov::InputSnapshot& in, float dt_ms) {
            RenderCommandList cmds;
            uif.Begin(in, theme, 400.0f, 400.0f, dt_ms, 0.0f);
            uif.Button("activate", box, baked);
            uif.End();
            uif.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 1u);
            return cmds.plate2d[0];
        };
        jpov::InputSnapshot idle{};
        idle.mouse_x = 350.0f;
        idle.mouse_y = 350.0f;
        const Plate2DCommand p = FrameF(idle, kDtMs);
        CHECK(std::fabs(p.border_width) < 1e-4f)
            << "kFlipbook 模式不该画边框带（否则整块图被挖掉一圈）："
            << p.border_width;
        CHECK_EQ(p.border.texture_id, 0u) << "kFlipbook 模式边框层不该采材质";
        CHECK_EQ(p.fill.texture_id, kCoreTex) << "整块底板必须是皮肤图集";
        CHECK_EQ(p.fill.flipbook.tiles_x, 4) << "整键图集的网格必须传下去";
        // material_px = 0 ⇒ 一格图铺满（tiling = 1，不平铺）。
        CHECK(std::fabs(p.fill.tiling.x() - 1.0f) < 1e-4f &&
              std::fabs(p.fill.tiling.y() - 1.0f) < 1e-4f)
            << "整键皮应当一格铺满：tiling=(" << p.fill.tiling.x() << ","
            << p.fill.tiling.y() << ")";
        // 圆角跟主题（本测试主题 6 ⇒ 设备像素仍是 6，scale=1）。
        CHECK(std::fabs(p.radius - theme.corner_radius_px) < 1e-4f)
            << "kFlipbook 模式的轮廓应跟主题圆角走";
        CheckTint(p, skin.normal_tint, "整键模式常态仍用 normal_tint");
        // 悬停一帧仍然只走到 25%：两种模式共用同一套渐变，不允许分叉。
        const Plate2DCommand h = FrameF(hover, kDtMs);
        CheckTint(h, Color{0.2f + 0.8f * 0.25f, 0.2f + 0.4f * 0.25f, 0.2f, 1.0f},
                  "整键模式的 hover 渐变应与板材模式一致（25%）");
        LOG(INFO) << "[PASS] kFlipbook 模式：无边框 / 一格铺满 / tint 语义同 kPlate";
    }

    // g) 跨帧回归：同一帧里同一个 box 被画两次（两颗按钮叠在同一点、或调用方
    //    不小心画了两遍）只能推进一次。以前的实现会按调用次数累加 dt ⇒ 渐变
    //    速度静默翻倍（0.25 → 0.5 一帧到半亮），肉眼很难发现是「时间尺度错了」。
    {
        SpUi uid;
        auto DoubleFrame = [&](const jpov::InputSnapshot& in, float dt_ms) {
            RenderCommandList cmds;
            uid.Begin(in, theme, 400.0f, 400.0f, dt_ms, 0.0f);
            uid.Button("a", box, skin);
            uid.Button("b", box, skin);   // 同 box：共享同一条 ratio 轨道
            uid.End();
            uid.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 2u) << "两颗按钮应各产出一条指令";
            return cmds.plate2d;
        };
        const auto first = DoubleFrame(hover, kDtMs);
        const Color want25{0.2f + 0.8f * 0.25f, 0.2f + 0.4f * 0.25f, 0.2f, 1.0f};
        CheckTint(first[0], want25, "同帧画两次：第一颗应 25%（不是 50%）");
        CheckTint(first[1], want25, "同帧画两次：第二颗应复用同一份 25%");
        LOG(INFO) << "[PASS] 同帧同 box 画两次：只推进一次（渐变不加速）";
    }

    // h) 跨帧回归：控件消失后轨道要被回收。否则隔几帧重新出现（换 tab/换面板）
    //    会带着旧的满亮 ratio 亮一帧，看起来像「没悬停却自己亮了」。
    {
        SpUi uir;
        auto HoverFrame = [&](const jpov::InputSnapshot& in) {
            RenderCommandList cmds;
            uir.Begin(in, theme, 400.0f, 400.0f, kDtMs, 0.0f);
            uir.Button("activate", box, skin);
            uir.End();
            uir.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 1u);
            return cmds.plate2d[0];
        };
        for (int i = 0; i < 4; ++i) HoverFrame(hover);   // 爬到满
        CheckTint(HoverFrame(hover), skin.hover_tint, "先确认已到满亮");
        // 4 帧完全不画这颗按钮（模拟换 tab / 面板重建）。
        for (int i = 0; i < 4; ++i) {
            RenderCommandList cmds;
            uir.Begin(hover, theme, 400.0f, 400.0f, kDtMs, 0.0f);
            uir.End();
            uir.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 0u) << "这些帧不该有任何控件";
        }
        // 重新出现 + 悬停：必须从 0 重新爬（25%），不能带着旧的 1.0。
        CheckTint(HoverFrame(hover),
                  Color{0.2f + 0.8f * 0.25f, 0.2f + 0.4f * 0.25f, 0.2f, 1.0f},
                  "消失 4 帧后轨道应被回收，重新从 25% 开始");
        LOG(INFO) << "[PASS] 消失 ≥3 帧后 hover 轨道被回收（不带旧状态回来）";
    }

    // i) 跨帧回归：运行时改 ui_scale 不该把所有 hover_ratio 清零。
    //    轨道若按「缩放后的设备像素」识别，改倍率后所有按钮都会当成新按钮，
    //    表现为「调整 UI 缩放时悬停状态闪一下」——很轻但确实是个跨帧 bug。
    {
        SpUi uis;
        auto HoverFrame = [&](const jpov::InputSnapshot& in) {
            RenderCommandList cmds;
            uis.Begin(in, theme, 400.0f, 400.0f, kDtMs, 0.0f);
            uis.Button("activate", box, skin);
            uis.End();
            uis.Emit(&cmds);
            CHECK_EQ(cmds.plate2d.size(), 1u);
            return cmds.plate2d[0];
        };
        for (int i = 0; i < 4; ++i) HoverFrame(hover);
        CheckTint(HoverFrame(hover), skin.hover_tint, "先跑到满亮");
        uis.SetUiScale(2.0f);
        const Plate2DCommand scaled = HoverFrame(hover);
        CheckTint(scaled, skin.hover_tint,
                  "改 ui_scale 后 ratio 不该归零（轨道按布局坐标识别）");
        // 再确认缩放真的生效了（否则上面这条会恒真）。
        CHECK(std::fabs(scaled.pos.x() - box.pos.x() * 2.0f) < 1e-4f)
            << "ui_scale=2 后板材位置应翻倍：" << scaled.pos.x();
        LOG(INFO) << "[PASS] 运行时改 ui_scale 不清空 hover_ratio（布局坐标识别 + 缩放确实生效）";
    }
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
    CheckLayerForScaleInvariance();
    CheckInteractionStateMachine();
    CheckButtonHoverRatio();
    CheckDegenerateInputs();
    LOG(INFO) << "===== sp_ui gold 全部通过 =====";
    return 0;
}

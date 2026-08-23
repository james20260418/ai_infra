// JPOV UI — 即时模式控件实现
//
// 本文件实现 ui.h 中声明的 Ui 类与 UiTheme。
//
// 状态机：每帧 Begin → 若干控件 → End（可选）→ Emit。
//   - Begin 记录本帧输入/主题/视口分辨率，并清空内部指令缓冲（从零构建）；
//   - 控件把绘制命令暂存到 fill_rects_ / texts_（Begin 收集）；
//   - Emit 一次性把内部缓冲追加到外部 RenderCommandList，然后清空缓冲。
//
// 本文件当前实现【S0 基础设施】骨架：Begin/End/Emit + 指令缓冲 +
// SanitizeBox/OutsideViewport/RowHeight + UiTheme::Default；
// S1 Text / ColorSwatch；S2 Button + Hit（命中/悬停/一次性点击）；
// S3 Checkbox（方框+勾、点击翻转外置 bool）；
// S4 SliderFloat（轨道+句柄+数值，点击跳转/拖动连续写回）。
// 其余控件（InputText/Combo）由后续子步骤（S5~S6）逐项实现。

#include "tools/jpov/interface/ui.h"

#include <algorithm>
#include <cstdio>

#include <glog/logging.h>

namespace jpov {

// ==================== UiTheme ====================

UiTheme UiTheme::Default(float font_size) {
    CHECK_GT(font_size, 0.0f) << "UiTheme font_size must be > 0";
    UiTheme t;
    t.font_size = font_size;

    // 现代扁平主题：深色面板底，浅色前景，青蓝色高亮。
    t.background = {0.13f, 0.14f, 0.16f, 1.0f};  // 面板底
    t.foreground = {0.92f, 0.93f, 0.95f, 1.0f};  // 文本/图标默认色
    t.accent     = {0.25f, 0.60f, 0.95f, 1.0f};  // 高亮（按钮主色/选中）
    t.hover      = {0.22f, 0.26f, 0.32f, 1.0f};  // 悬停态填充
    t.border     = {0.35f, 0.37f, 0.42f, 1.0f};  // 边框
    t.disabled   = {0.45f, 0.46f, 0.50f, 1.0f};  // 禁用态文字/图标

    t.padding_px       = 6.0f;
    t.spacing_px       = 4.0f;
    t.corner_radius_px = 4.0f;
    t.border_width_px  = 1.0f;
    return t;
}

// ==================== Ui 生命周期 ====================

void Ui::Begin(const InputSnapshot& input, const UiTheme& theme,
               float width, float height) {
    CHECK_GE(width, 0.0f) << "Ui viewport width must be >= 0";
    CHECK_GE(height, 0.0f) << "Ui viewport height must be >= 0";
    input_ = &input;
    theme_ = theme;
    width_ = width;
    height_ = height;
    // 每帧从零构建：清空上一帧收集的指令。
    fill_rects_.clear();
    texts_.clear();
    polylines_.clear();
}

void Ui::End() {
    // 本帧控件收集到此为止。Emit 会隐式结束本帧，此处是显式结束的钩子。
    // S0 阶段无跨帧状态需要结算，保留为空实现以对称 Begin/End 生命周期。
}

void Ui::Emit(RenderCommandList* cmd /*output*/) {
    CHECK(cmd != nullptr);
    // 把本帧内部缓冲一次性追加到外部 RenderCommandList（不清空外部已有内容）。
    for (const FillRect2DCommand& r : fill_rects_) {
        cmd->DrawFillRect(r.pos, r.size, r.fill_color, r.border_color,
                          r.border_width, r.radius);
    }
    for (const Text2DCommand& t : texts_) {
        cmd->DrawText(t.text, t.pos, t.font_size, t.color, t.alignment,
                      t.font_alias);
    }
    for (const Polyline2DCommand& p : polylines_) {
        cmd->DrawPolyline(p.vertices, p.color, p.line_width);
    }
    // 追加完成后清空内部缓冲，本帧结束。
    fill_rects_.clear();
    texts_.clear();
    polylines_.clear();
}

// ==================== 基础控件（S1：Text / ColorSwatch） ====================

void Ui::Text(const char* label, const UiRect& box, Stretch /*stretch*/) {
    if (label == nullptr || label[0] == '\0') {
        return;  // 空标签不画。
    }
    // 规格化 box（负尺寸 clamp 0），越界/零尺寸则跳过。
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return;
    }
    Text2DCommand c;
    c.text = label;
    // 容错 #1：字形保持原尺寸（theme_.font_size），绝不缩小。
    // 用 kCenter 对齐把字形的实际包围盒中心放到 box 中心 →
    // 原尺寸文字在 box 内垂直（与水平）居中；box 过小时文字仍原尺寸，
    // 超出 box 的部分由渲染层/视口兜底（本接口不做 scissor）。
    c.pos = b.pos + b.size * 0.5f;
    c.font_size = theme_.font_size;
    c.color = theme_.foreground;
    c.alignment = TextAlignment::kCenter;
    c.font_alias.clear();
    texts_.push_back(c);
}

void Ui::ColorSwatch(const char* /*label*/, const Color& color, const UiRect& box,
                     Stretch /*stretch*/) {
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return;  // 越界/零尺寸跳过。
    }
    // 容错 #3：真方形控件。box 不足正方形时按 min(宽,高) 取正方形，
    // 在 box 内居中（box 本身正方形时 side=min=box 边长，即铺满 box）。
    const float side = std::min(b.size.x(), b.size.y());
    if (side <= 0.0f) {
        return;
    }
    const UiRect square{
        {b.pos.x() + (b.size.x() - side) * 0.5f,
         b.pos.y() + (b.size.y() - side) * 0.5f},
        {side, side},
    };
    // PushFillRect 内部会做越界剔除、零面积跳过、圆角 clamp。
    PushFillRect(square, color, theme_.border, theme_.corner_radius_px);
}

bool Ui::Button(const char* label, const UiRect& box, Stretch /*stretch*/) {
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 越界/零尺寸：不画、不命中。
    }

    // 命中测试：Ui root 面板默认位于窗口原点 (0,0)（ui.h 约定：若面板位移，
    // 由调用方自行做一次盒偏移）。hovered 决定是否进入悬停态（S2.3）。
    const InputSnapshot& in = *input_;
    const bool hovered = Hit(b, 0.0f, 0.0f, in);

    // 背景：悬停变色。accent 为按钮主色，悬停用 hover 填充色。
    const Color fill = hovered ? theme_.hover : theme_.accent;
    PushFillRect(b, fill, theme_.border, theme_.corner_radius_px);
    // 文本：原字号、box 中心对齐（复用 Text 的居中语义）。
    Text(label, b);

    // 点击返回 true（S2.4）：本帧左键有 Click 事件，且释放位置落在按钮 box 内
    // → 一次性事件（仅当帧返回 true，不存在跨帧记忆）。
    if (in.left.IsClick()) {
        for (int i = 0; i < in.left.click_count(); ++i) {
            const float cx = in.left_clicks[i].x;
            const float cy = in.left_clicks[i].y;
            if (cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
                cy >= b.pos.y() && cy <= b.pos.y() + b.size.y()) {
                return true;
            }
        }
    }
    return false;
}

bool Ui::Checkbox(const char* label, bool* value, const UiRect& box,
                 Stretch /*stretch*/) {
    // 状态外置：value 必须由调用方提供（本接口不跨帧记忆）。
    CHECK(value != nullptr) << "Checkbox value pointer must not be null";
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 越界/零尺寸：不画、不命中。
    }

    // 点击翻转（一次性事件，同 Button）：本帧左键 Click + 释放位置落在
    // box 内 → 翻转 *value 并返回 true（不跨帧记忆，下一帧从零读 *value）。
    const InputSnapshot& in = *input_;
    bool clicked = false;
    if (in.left.IsClick()) {
        for (int i = 0; i < in.left.click_count(); ++i) {
            const float cx = in.left_clicks[i].x;
            const float cy = in.left_clicks[i].y;
            if (cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
                cy >= b.pos.y() && cy <= b.pos.y() + b.size.y()) {
                *value = !*value;  // 点击 → 翻转外置状态。
                clicked = true;
                break;
            }
        }
    }

    // 真方形框（容错 #3）：box 不足正方形时按 min(宽,高) 取正方形、居中。
    const float side = std::min(b.size.x(), b.size.y());
    if (side <= 0.0f) {
        return clicked;  // 零尺寸不画（但也已处理上面的点击翻转）。
    }
    const UiRect square{
        {b.pos.x() + (b.size.x() - side) * 0.5f,
         b.pos.y() + (b.size.y() - side) * 0.5f},
        {side, side},
    };
    // 方框底：选中=accent 高亮，未选=面板底。边框统一 border。
    const Color fill = (*value) ? theme_.accent : theme_.background;
    PushFillRect(square, fill, theme_.border, theme_.corner_radius_px);

    // 勾选标记（仅选中态）：在方形内画一个 2 段折线 “✓”。
    // 折线顶点取方形内缩 padding 的“左中→右下→右上”三点，形成对勾。
    if (*value) {
        const float pad = std::min(theme_.padding_px, side * 0.25f);
        const float x0 = square.pos.x() + pad;
        const float y0 = square.pos.y() + pad;
        const float w = square.size.x() - 2.0f * pad;
        const float h = square.size.y() - 2.0f * pad;
        std::vector<Vec2f> verts = {
            {x0, y0 + h * 0.62f},
            {x0 + w * 0.36f, y0 + h * 0.90f},
            {x0 + w, y0 + h * 0.18f},
        };
        const float lw = std::max(1.0f, side * 0.12f);
        PushPolyline(verts, theme_.foreground, lw);
    }

    // 标签文本：原字号、box 中心（复用 Text 语义，画在方框之后上方）。
    Text(label, b);
    return clicked;
}

bool Ui::SliderFloat(const char* label, float* value, const UiRect& box,
                     float min, float max, Stretch /*stretch*/) {
    // min < max 是前置条件；不满足则无法定义归一化映射，直接 crash 暴露问题
    // （不做 fallback 隐藏配置错误，遵循仓库 crash-early 原则）。
    CHECK(value != nullptr) << "SliderFloat value pointer must not be null";
    CHECK_LT(min, max) << "SliderFloat requires min < max, got (" << min
                       << ", " << max << ")";
    const float range = max - min;

    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 越界/零尺寸：不画、不命中。
    }

    // ---- 值域处理：外置 value 先夹到 [min,max]（防调用方越界污染）----
    // 立即把夹断后的值写回 *value，保证显示与状态一致（每次绘制后
    // *value 都落在 [min,max]，不存在显示 100 而状态 999 的分歧）。
    const float clamped = std::clamp(*value, min, max);
    *value = clamped;

    // ---- 布局：轨道 + 句柄 ----
    // 轨道：横向铺满 box（左右留一个句柄半径的边距，避免句柄溢出 box 边缘），
    // 垂直居中、厚度固定取 box 高的 30%（至少 4px）。
    // 句柄直径：clamp ≥ min_diameter（容错 #2，默认 8px），且不超出 box 宽
    // （窄 box 句柄取 min(8, 宽)，保证不缩 0、不溢出 box）。
    // 注意：不能直接用 std::clamp(b.size.y(), 8, b.size.x())——矮而窄的 box
    // 会出现 lo>hi 的未定义行为，故先 max 抬底再 min 封顶。
    constexpr float kMinHandle = 8.0f;
    const float thickness = std::max(4.0f, b.size.y() * 0.30f);
    const float handle_d =
        std::min(std::max(b.size.y(), kMinHandle), b.size.x());
    // 句柄中心可移动的水平范围（轨道去两端句柄半宽）。
    const float half = handle_d * 0.5f;
    const float track_left = b.pos.x() + half;
    const float track_right = b.pos.x() + b.size.x() - half;
    // 轨道长度：极窄 box（句柄已几乎占满）时仍保证轨道有最小正宽度，
    // 满足容错“box 过窄轨道照画、不消失”（避免 track 退化为 0 面积被剔除）。
    const float track_len = std::max(1.0f, track_right - track_left);
    // 归一化位置 [0,1]：value 映射到轨道长度。
    const float t = (track_len > 0.0f) ? (clamped - min) / range : 0.0f;
    const float hx = track_left + t * track_len;   // 句柄中心 X
    const float cy = b.pos.y() + b.size.y() * 0.5f;  // 垂直中心

    // 轨道矩形（垂直居中，厚 thickness）。
    const UiRect track_rect{
        {track_left, cy - thickness * 0.5f},
        {track_len, thickness},
    };
    // 句柄矩形（方形，边长 handle_d，垂直居中）。
    const UiRect handle_rect{
        {hx - half, cy - half},
        {handle_d, handle_d},
    };

    // ---- 交互（S4.2 拖动）：命中 → 横坐标比例映射 [min,max] 写回 ----
    // 即时模式、不跨帧记忆：本帧若有左键按下且鼠标在滑条 box 内，就持续把
    // 当前鼠标横坐标映射到值。这样 Click（点轨道跳转）与 Drag（拖句柄跟手）
    // 都能实时反映；返回 true = 本帧值被改变（一次性事件）。
    const InputSnapshot& in = *input_;
    const float lx = in.mouse_x - 0.0f;   // root 面板位于窗口原点 (0,0)（ui.h 约定）。
    const bool mouse_over =
        (lx >= b.pos.x()) && (lx <= b.pos.x() + b.size.x());
    const bool left_active = in.left.IsDrag() || in.left.IsHold();

    bool changed = false;
    if (mouse_over && left_active) {
        // 拖动/按住期间，跟随鼠标横坐标（点击即跳到该处）。
        const float nx =
            (track_len > 0.0f) ? (lx - track_left) / track_len : 0.0f;
        const float new_val =
            min + std::clamp(nx, 0.0f, 1.0f) * range;
        if (new_val != clamped) {
            *value = new_val;
            changed = true;
        }
    } else if (in.left.IsClick()) {
        // 单击：仅当释放位置落在滑条 box 内才写值（一次性事件，同 Button）。
        for (int i = 0; i < in.left.click_count(); ++i) {
            const float cx = in.left_clicks[i].x;
            const float cy2 = in.left_clicks[i].y;
            if (cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
                cy2 >= b.pos.y() && cy2 <= b.pos.y() + b.size.y()) {
                const float nx =
                    (track_len > 0.0f) ? (cx - track_left) / track_len : 0.0f;
                *value = min + std::clamp(nx, 0.0f, 1.0f) * range;
                changed = true;
                break;  // 只取首个落在 box 内的 click。
            }
        }
    }

    // ---- 绘制（S4.1）：轨道 + 句柄 + 数值 ----
    // 轨道底(背景) + 选中行程(accent，min→句柄) + 句柄 + 数值文本。
    PushFillRect(track_rect, theme_.background, theme_.border,
                 theme_.corner_radius_px);
    // 选中行程：min→句柄 用 accent 高亮（宽度 = 句柄中心 - 轨道左端）。
    const float fill_len = std::max(0.0f, hx - track_left);
    if (fill_len > 0.0f) {
        PushFillRect(UiRect{{track_left, cy - thickness * 0.5f},
                            {fill_len, thickness}},
                     theme_.accent, theme_.accent, theme_.corner_radius_px);
    }
    // 句柄：实心方框（accent 前景色）+ 边框，独立于轨道可见。
    PushFillRect(handle_rect, theme_.foreground, theme_.border,
                 theme_.corner_radius_px);

    // 数值文本：显示 label 与当前整数化取值（如 “speed: 50”）。
    // 复用 Text 的居中/原字号语义（画在轨道之上 box 中心）。
    if (label != nullptr && label[0] != '\0') {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %.0f", label, *value);
        std::string text(buf);
        // 直接构造一个居中文本指令（Text 原字号、box 中心，复用其语义）。
        Text2DCommand c;
        c.text = text;
        c.pos = b.pos + b.size * 0.5f;
        c.font_size = theme_.font_size;
        c.color = theme_.foreground;
        c.alignment = TextAlignment::kCenter;
        c.font_alias.clear();
        texts_.push_back(c);
    }
    return changed;
}

bool Ui::Hit(const UiRect& r, float x, float y, const InputSnapshot& in) {
    const UiRect b = SanitizeBox(r);
    if (b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 零尺寸矩形不参与命中。
    }
    // (x, y) = Ui root 面板左上角在窗口中的位置（默认 0,0）；in.mouse 为窗口
    // 绝对坐标。先转成面板局部坐标，再判点是否落在 box 内（边界含入）。
    const float lx = in.mouse_x - x;
    const float ly = in.mouse_y - y;
    return (lx >= b.pos.x()) && (lx <= b.pos.x() + b.size.x()) &&
           (ly >= b.pos.y()) && (ly <= b.pos.y() + b.size.y());
}

UiRect Ui::SanitizeBox(const UiRect& r) {
    UiRect out = r;
    // Vec2f::x()/y() 返回引用，可直接赋值（clamp 负尺寸到 0）。
    out.size.x() = std::max(0.0f, out.size.x());
    out.size.y() = std::max(0.0f, out.size.y());
    return out;
}

bool Ui::OutsideViewport(const UiRect& r) const {
    // 空尺寸矩形（宽或高为 0）视为不占据面积 → 判定为"在视口外"跳过，
    // 避免零面积矩形参与相交测试产生歧义。
    if (r.size.x() <= 0.0f || r.size.y() <= 0.0f) {
        return true;
    }
    // AABB 不相交测试：矩形完全落在视口 (0,0,width,height) 之外即为越界。
    const float left   = r.pos.x();
    const float top    = r.pos.y();
    const float right  = left + r.size.x();
    const float bottom = top + r.size.y();
    return (right <= 0.0f) || (bottom <= 0.0f) ||
           (left >= width_) || (top >= height_);
}

float Ui::RowHeight() const {
    // 内容感知行高：字形像素高度 + 上下内边距。
    // 首版固定近似（glyph 平均高度 + 内边距），后续接入字体度量 API。
    return theme_.font_size + 2.0f * theme_.padding_px;
}

// ==================== 内部绘制原语 ====================

void Ui::PushFillRect(const UiRect& r, Color fill, Color border, float radius) {
    // 规格化 box（负尺寸 clamp 0），圆角 clamp 到合法上限。
    const UiRect box = SanitizeBox(r);
    if (OutsideViewport(box)) {
        return;  // 越界剔除：完全不与视口相交则跳过。
    }
    if (box.size.x() <= 0.0f || box.size.y() <= 0.0f) {
        return;  // 零面积矩形不画。
    }
    const float half_min = std::min(box.size.x(), box.size.y()) * 0.5f;
    const float safe_radius = std::clamp(radius, 0.0f, half_min);
    const float safe_border = std::max(0.0f, theme_.border_width_px);

    FillRect2DCommand c;
    c.pos = box.pos;
    c.size = box.size;
    c.fill_color = fill;
    c.border_color = border;
    c.border_width = safe_border;
    c.radius = safe_radius;
    fill_rects_.push_back(c);
}

void Ui::PushText(const char* s, const UiRect& box) {
    if (s == nullptr) {
        return;
    }
    // 规格化 box：负尺寸 clamp 0；零尺寸文本不画。
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return;
    }
    Text2DCommand c;
    c.text = s;
    // 首版：按包围盒左上角对齐（TextAlignment::kTopLeft）。
    // 具体字形度量 / 垂直居中 / 裁切（S1 文本绘制）留待后续子步骤完善，
    // 此处先把 box 左上角写入，保证缓冲链路完整。
    c.pos = b.pos;
    c.font_size = theme_.font_size;
    c.color = theme_.foreground;
    c.alignment = TextAlignment::kTopLeft;
    c.font_alias.clear();
    texts_.push_back(c);
}

void Ui::PushPolyline(const std::vector<Vec2f>& vertices, Color color,
                      float line_width) {
    if (vertices.size() < 2) {
        return;  // 折线至少需要 2 个顶点。
    }
    Polyline2DCommand c;
    c.vertices = vertices;
    c.color = color;
    c.line_width = std::max(0.0f, line_width);
    polylines_.push_back(c);
}

}  // namespace jpov

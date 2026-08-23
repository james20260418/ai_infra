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
// S4 SliderFloat（轨道+句柄+数值，点击跳转/拖动连续写回）；
// S5 InputText（底框+光标+占位符，点击聚焦+键盘写回 char*、限容量+超长水平滚动）；
// S6 Combo（当前项+下箭头、点击展开列表选择写回/点外部关闭、跨帧展开态）。
// 其余控件（集成 demo 等）由后续子步骤（S7+）实现。

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

// S5 键盘字符映射：KeyCode → 可编辑字符（与 ui.h 声明一致）。
char UiInputCharForKey(KeyCode key) {
    // 字母表 a-z（InputSnapshot 无 Shift 修饰，一律小写）。
    if (key >= KeyCode::A && key <= KeyCode::Z) {
        return static_cast<char>('a' + (static_cast<int>(key) - static_cast<int>(KeyCode::A)));
    }
    // 数字 0-9。
    if (key >= KeyCode::_0 && key <= KeyCode::_9) {
        return static_cast<char>('0' + (static_cast<int>(key) - static_cast<int>(KeyCode::_0)));
    }
    switch (key) {
        case KeyCode::Space:
            return ' ';
        default:
            return '\0';  // 修饰键/控制键/方向键等：不可编辑字符，调用方忽略。
    }
}

bool Ui::InputText(const char* label, char* buffer, size_t buffer_size,
                   const UiRect& box, Stretch /*stretch*/) {
    // buffer 必须非空且至少留 1 字节存 '\0'（容量下限 2：1 字符 + 终止符）。
    CHECK(buffer != nullptr) << "InputText buffer must not be null";
    CHECK_GE(buffer_size, 2u) << "InputText requires buffer_size >= 2 "
                              << "(1 char + null terminator), got " << buffer_size;
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 越界/零尺寸：不画、不聚焦、不写值。
    }

    // 确保 buffer 以 '\0' 收尾（防调用方传入非终止字符串污染长度计算）。
    // buffer 至少 buffer_size 字节，下标 buffer_size-1 必然合法。
    buffer[buffer_size - 1] = '\0';
    const size_t text_len = strnlen(buffer, buffer_size - 1);

    // ---- 焦点管理（S5.2）：状态跨帧，点击 box 内获得焦点 ----
    // 即时模式：focus 是本对象跨帧状态。本帧绘制时，若本 box 与上次聚焦 box
    // 位置+尺寸一致 → 视为已聚焦（框移动/临时对象时自动失焦，重新点击才聚焦）。
    const InputSnapshot& in = *input_;
    const bool is_focused =
        input_focused_ && input_focus_box_.pos == b.pos &&
        input_focus_box_.size == b.size;

    // 本帧点击落在 box 内 → 聚焦本框。
    bool clicked_inside = false;
    if (in.left.IsClick()) {
        for (int i = 0; i < in.left.click_count(); ++i) {
            const float cx = in.left_clicks[i].x;
            const float cy = in.left_clicks[i].y;
            if (cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
                cy >= b.pos.y() && cy <= b.pos.y() + b.size.y()) {
                clicked_inside = true;
                break;
            }
        }
    }
    // 聚焦状态结算（先做，供下方键入判断使用）：
    //  - 点本框 → 聚焦；点别处 → 若聚焦正是本框则失焦。
    if (clicked_inside) {
        input_focused_ = true;
        input_focus_box_ = b;
        input_scroll_px_ = 0.0f;  // 聚焦时滚动归零（从头显示）。
    } else if (in.left.IsClick() && is_focused) {
        input_focused_ = false;  // 本帧 Click 落在 box 外 → 本框失焦。
    }

    // ---- 键盘写回 char*（S5.2）：仅聚焦框消费键入 ----
    // 用本轮聚焦判定后的 is_focused 快照决定是否消费按键，避免同帧内
    // 焦点迁移（点框外导致失焦）后仍把字符打进刚失焦的 buffer。
    // 可编辑长度用可变变量跟踪（追加/删除会改变它）。
    size_t len = text_len;
    const size_t cap = buffer_size - 1;  // 可容纳的最大字符数（留 '\0'）。
    if (is_focused) {
        // 控制键：Enter / Escape → 失焦（提交/取消，都不改变 buffer）。
        const KeyState& enter = in.GetKey(KeyCode::Enter);
        const KeyState& esc = in.GetKey(KeyCode::Escape);
        if (enter.IsClick() || esc.IsClick()) {
            input_focused_ = false;
        } else {
            // Backspace：删除最后一个字符（删除发生在追加前）。
            const KeyState& bs = in.GetKey(KeyCode::Backspace);
            if (bs.IsClick()) {
                const size_t n =
                    std::min(static_cast<size_t>(bs.click_count()), len);
                for (size_t i = 0; i < n; ++i) {
                    buffer[len - i - 1] = '\0';
                }
                len -= n;
                buffer[len] = '\0';
            }
            // 可编辑字符：从 KeyCode 读，逐字符追加，遇容量上限截断（S5.2）。
            // 按码点序扫描（Space..Z，含 a-z / 0-9 / 空格区间）。多余连击（
            // 低帧率同帧多次按下）按次追加，容量满则丢弃余量（截断）。
            for (int code = static_cast<int>(KeyCode::Space);
                 code <= static_cast<int>(KeyCode::Z); ++code) {
                const KeyState& ks = in.GetKey(static_cast<KeyCode>(code));
                if (!ks.IsClick()) {
                    continue;
                }
                const char ch = UiInputCharForKey(static_cast<KeyCode>(code));
                if (ch == '\0') {
                    continue;  // 不可编辑字符跳过。
                }
                const size_t n = std::min(static_cast<size_t>(ks.click_count()),
                                          cap - len);
                for (size_t i = 0; i < n; ++i) {
                    buffer[len + i] = ch;
                }
                len += n;
                buffer[len] = '\0';
            }
        }
    }

    // 绘制/返回用的“当前聚焦”判定：必须在本帧全部输入处理（含 Enter/Escape
    // 失焦）之后结算，才能正确反映本帧点击聚焦/框外失焦/回车失焦的最终状态。
    // 键盘写回用的是上面的 is_focused 快照，二者语义不同：
    //   键入：用“帧首是否已聚焦”决定是否消费（点击聚焦那一刻不消费，下帧才打字）；
    //   绘制/返回：用“帧末最终聚焦”决定画不画光标、返回值。
    const bool focused_eff =
        input_focused_ && input_focus_box_.pos == b.pos &&
        input_focus_box_.size == b.size;

    // ---- 绘制（S5.1）：底框 + 占位符/内容文本 + 光标 ----
    // 底框：background 底 + border 边框（与其它控件一致的圆角/边框语义）。
    PushFillRect(b, theme_.background, theme_.border, theme_.corner_radius_px);

    // 文本内容区（左侧留 padding 内边距，垂直居中）。
    const float pad = theme_.padding_px;
    const float text_left = b.pos.x() + pad;
    const float cy = b.pos.y() + b.size.y() * 0.5f;  // 垂直中心（文本与光标共用）

    if (len > 0) {
        // 内容文本：foreground 色、左对齐、垂直居中（kMidLeft，pos=左缘中点）。
        // 手动压一条 Text2DCommand（与 S4 滑条数值文本一致），而非 PushText
        //（PushText 仅 kTopLeft，无法垂直居中，与光标不对齐）。
        Text2DCommand c;
        c.text = buffer;
        c.pos = {text_left, cy};
        c.font_size = theme_.font_size;
        c.color = theme_.foreground;
        c.alignment = TextAlignment::kMidLeft;
        c.font_alias.clear();
        texts_.push_back(c);
    } else if (label != nullptr && label[0] != '\0') {
        // 空 buffer → 画占位符（提示输入内容），disabled 色以示与正文区分。
        Text2DCommand c;
        c.text = label;
        c.pos = {text_left, cy};
        c.font_size = theme_.font_size;
        c.color = theme_.disabled;
        c.alignment = TextAlignment::kMidLeft;
        c.font_alias.clear();
        texts_.push_back(c);
    }

    // 光标（S5.1/S5.3）：聚焦时在文本末尾画一条竖线（静态，不闪烁保 gold 可测）。
    // 水平滚动：文本末位估计宽度超过 box 右缘（去 padding）→ 推进内部滚动，
    // 保证光标不越出右缘（S5.3 内部 scroll、不溢出）。无字体度量 API，字符宽
    // 用等宽近似（字号 * 0.6）参与定位与滚动判断；实际字形仍按完整串交渲染层。
    if (focused_eff) {
        const float caret_w = std::max(1.0f, theme_.border_width_px);
        const float char_w = theme_.font_size * 0.6f;
        const float caret_right = b.pos.x() + b.size.x() - pad;
        // 光标相对内容左缘的原始位置 = 文本总估计宽度 - 当前滚动偏移。
        const float caret_x =
            text_left + static_cast<float>(len) * char_w - input_scroll_px_;
        if (caret_x > caret_right) {
            // 越过右缘：滚动多出的量，使光标保持恰在右缘内侧。
            input_scroll_px_ += caret_x - caret_right;
        }
        // 最终光标 X：贴右缘内侧，绝不越界。
        const float cx =
            std::min(text_left + static_cast<float>(len) * char_w, caret_right);
        const float caret_h = std::max(2.0f, b.size.y() * 0.7f);
        PushFillRect(UiRect{{cx, cy - caret_h * 0.5f}, {caret_w, caret_h}},
                     theme_.accent, theme_.accent, 0.0f);
    }

    // 返回本帧聚焦结果（反映本帧点击/失焦处理后的最终状态，供调用方在
    // 回车/取消时感知失焦事件；键入本身通过写回 buffer 反映，无需关心）。
    return focused_eff;
}

bool Ui::Combo(const char* label, int* selected,
               const std::vector<const char*>& items, const UiRect& box,
               Stretch /*stretch*/) {
    // selected 为 in/out，合法范围 [0, items.size()-1]（任务验收备注）。
    CHECK(selected != nullptr) << "Combo selected pointer must not be null";
    const UiRect b = SanitizeBox(box);
    if (OutsideViewport(b) || b.size.x() <= 0.0f || b.size.y() <= 0.0f) {
        return false;  // 越界/零尺寸：不画、不展开、不改值。
    }

    // ---- 值域处理：选中的 index 先夹到 [0, size-1]（防调用方越界污染）----
    // 立即把夹断后的值写回 *selected，保证显示与状态一致。
    const int size = static_cast<int>(items.size());
    if (size == 0) {
        *selected = -1;  // 无选项 → 无选中（显示占位符）。
    } else {
        *selected = std::clamp(*selected, 0, size - 1);
    }

    // ---- 跨帧展开状态（同 InputText 焦点模式）：用 box 位置+尺寸识别同一
    // Combo；本帧绘制时若 box 与之相等则视为已展开。----
    const InputSnapshot& in = *input_;
    const bool was_open =
        combo_open_ && combo_open_box_.pos == b.pos &&
        combo_open_box_.size == b.size;

    // ---- 布局：组合框 + 下拉列表 + 下箭头（仅在框内画，下拉列表不画箭头）----
    const float row_h = RowHeight();
    const float pad = theme_.padding_px;
    const float cy = b.pos.y() + b.size.y() * 0.5f;  // 框垂直中心（文本对齐用）

    // 下箭头（▾）：右侧、垂直居中。用 3 顶点朝下三角形折线绘制，纯几何
    // 不依赖字形，gold 自证可确定性比对。尺寸取 box 高的一半（clamp 到合法
    // 范围），右侧留 padding 边距。
    const float arrow_h = std::max(2.0f, std::min(b.size.y() * 0.5f, 12.0f));
    const float arrow_w = arrow_h;  // 三角形底宽 = 高（等边倾向）
    const float ar = b.pos.x() + b.size.x() - pad - arrow_w;  // 箭头右上 x
    const float ay = cy - arrow_h * 0.5f;

    // 下拉列表：组合框正下方，宽度=框宽，行数=选项数。
    const float list_top = b.pos.y() + b.size.y();
    const float list_h = static_cast<float>(size) * row_h;

    // ---- 交互（S6.2）：展开/收起 + 选值 + 点外部关闭 ----
    bool changed = false;   // 本帧是否发生了选项切换（返回值）。
    bool open_now = was_open;  // 本帧绘制/返回用的最终展开态。
    if (in.left.IsClick()) {
        const float cx = in.left_clicks[0].x;  // 只取首个 click 位置决定展开行为。
        const float cy2 = in.left_clicks[0].y;
        const bool in_box =
            cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
            cy2 >= b.pos.y() && cy2 <= b.pos.y() + b.size.y();

        if (in_box) {
            // 点击组合框本身：开→关、关→开（点框内不产生选值，仅切换展开态）。
            open_now = !was_open;
        } else if (was_open && size > 0) {
            // 已展开且点框外：检查是否命中某个选项行 → 选中并关闭；
            // 否则（点空白/别处）→ 关闭不选值（S6.2 点外部关闭）。
            for (int i = 0; i < size; ++i) {
                const float row_y = list_top + static_cast<float>(i) * row_h;
                if (cx >= b.pos.x() && cx <= b.pos.x() + b.size.x() &&
                    cy2 >= row_y && cy2 <= row_y + row_h) {
                    *selected = i;  // 选中写回（唯一改变 *selected 的路径）。
                    changed = true;
                    break;
                }
            }
            open_now = false;  // 无论命中选项还是点外部空白，点击都关闭下拉。
        }
        // was_open == false 且点框外：保持关闭，不改值。
    }

    // 结算跨帧展开状态（供下一帧 was_open 判定）。
    combo_open_ = open_now;
    combo_open_box_ = b;

    // ---- 绘制（S6.1：当前项 + 下箭头 / 展开时 + 下拉列表）----
    // 组合框底：accent 高亮（展开时）否则 background，统一 border 边框。
    const Color box_fill = open_now ? theme_.hover : theme_.background;
    PushFillRect(b, box_fill, theme_.border, theme_.corner_radius_px);

    // 当前项文本：左对齐垂直居中（左侧留 padding）。选中时显示当前项；
    // 无选项（空 items）→ 显示 label 作占位符（disabled 色）。
    const char* current = nullptr;
    if (size > 0 && *selected >= 0 && *selected < size) {
        current = items[*selected];
    }
    if (current != nullptr && current[0] != '\0') {
        PushLabelText(current, b.pos.x() + pad, cy, theme_.foreground);
    } else if (label != nullptr && label[0] != '\0') {
        PushLabelText(label, b.pos.x() + pad, cy, theme_.disabled);
    }

    // 下箭头：仅收起态画（展开时被下拉列表盖住，语义上无需再提示）。
    // 用 3 顶点朝下三角形折线（不依赖字体字形，gold 可确定性比对）。
    if (!open_now && size > 0) {
        const std::vector<Vec2f> tri = {
            {ar, ay},
            {ar + arrow_w, ay},
            {ar + arrow_w * 0.5f, ay + arrow_h},
        };
        PushPolyline(tri, theme_.foreground, std::max(1.0f, theme_.border_width_px));
    }

    // 下拉列表（仅展开态）：背景 + 每行选项文本（当前项高亮）。
    if (open_now && size > 0) {
        PushFillRect(UiRect{{b.pos.x(), list_top}, {b.size.x(), list_h}},
                     theme_.background, theme_.border, theme_.corner_radius_px);
        for (int i = 0; i < size; ++i) {
            const float row_y = list_top + static_cast<float>(i) * row_h;
            const UiRect row{{b.pos.x(), row_y}, {b.size.x(), row_h}};
            if (OutsideViewport(row) || row.size.y() <= 0.0f) {
                continue;  // 行完全越出视口则跳过（底部超出部分 GPU 兜底）。
            }
            if (i == *selected) {
                // 当前项：accent 高亮底色 + foreground 文本。
                PushFillRect(row, theme_.accent, theme_.border,
                             theme_.corner_radius_px);
            }
            const char* text = items[i];
            if (text != nullptr && text[0] != '\0') {
                const float row_cy = row_y + row_h * 0.5f;
                // 当前项底色已用 accent 高亮，文本统一用 foreground 即可区分。
                PushLabelText(text, b.pos.x() + pad, row_cy, theme_.foreground);
            }
        }
    }

    // 返回本帧是否发生了选项切换（展开/收起本身不计为选值）。
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

void Ui::PushLabelText(const char* s, float left, float cy, Color color) {
    if (s == nullptr || s[0] == '\0') {
        return;  // 空串不画。
    }
    Text2DCommand c;
    c.text = s;
    // 左对齐 + 垂直居中：pos = 左缘中点（TextAlignment::kMidLeft）。
    // 不在此做越界/零尺寸判定（文本无盒概念，交由渲染层兜底）。
    c.pos = {left, cy};
    c.font_size = theme_.font_size;
    c.color = color;
    c.alignment = TextAlignment::kMidLeft;
    c.font_alias.clear();
    texts_.push_back(c);
}

}  // namespace jpov

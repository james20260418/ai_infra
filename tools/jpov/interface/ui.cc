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
// SanitizeBox/OutsideViewport/RowHeight + UiTheme::Default。
// 具体控件（Text/Button/Checkbox/SliderFloat/InputText/Combo/ColorSwatch）
// 由后续子步骤（S1~S6）逐项实现。

#include "tools/jpov/interface/ui.h"

#include <algorithm>

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
    // 追加完成后清空内部缓冲，本帧结束。
    fill_rects_.clear();
    texts_.clear();
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

// ==================== 容错辅助 ====================

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

}  // namespace jpov

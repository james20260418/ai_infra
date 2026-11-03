// JPOV sp_ui 输入框「选区 + 剪贴板」测试（纯 CPU，无窗口、无 GL）
//
// 覆盖 Danis 2026-11-03 提的两批功能：
//   第一批：左键拖拽选字 / 选中后删除与输入行为变化 / 框内再点取消选中 /
//           方向键把选区折叠成单竖杠。
//   第二批：Ctrl+A / X / C / V（含中文粘贴、UTF-8 码点安全、单行控制字符清理）。
//
// 为什么断言全部落在 **buffer 内容 + 发出的 2D 指令** 上：
//   SpUi 是纯 CPU 指令层，buffer 就是“用户看到并会保存的东西”；选区/光标
//   是否画对则由指令表证明（选区是一块 alpha < 1 的板，光标是一条竖板）。
//   这样不依赖字体/GPU，跑得比任何截图 diff 都快，也更容易读失败原因。
//
// 坐标系：theme 用 SpTheme::Default(16)（padding = 6，ASCII 回退宽度 0.6em =
// 9.6px）。要“点在某个字符边界附近”，x 由 CharX(i) 算，与控件内部同一套回退
// 度量 —— 测的是编辑逻辑，不是字体。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/sp_ui/sp_ui.h"

namespace {

using jpov::InputSnapshot;
using jpov::KeyCode;
using jpov::sp_ui::SpRect;
using jpov::sp_ui::SpTheme;
using jpov::sp_ui::SpUi;

constexpr float kWidth = 400.0f;
constexpr float kHeight = 200.0f;
constexpr float kFontSize = 16.0f;
constexpr float kPad = 6.0f;                       // SpTheme::Default 的内边距
constexpr SpRect kBox = {{20.0f, 20.0f}, {240.0f, 32.0f}};
constexpr float kTextLeft = kBox.pos.x() + kPad;   // 26
constexpr float kRowCy = kBox.pos.y() + kBox.size.y() * 0.5f;  // 36

// 第 i 个 ASCII 字符的右边界的 x（回退度量：0.6em/字符）。
float CharX(int i) {
    return kTextLeft + kFontSize * 0.6f * static_cast<float>(i);
}

// 一帧：op 用来改这份快照（按键 / 修饰键）。
InputSnapshot Base() {
    InputSnapshot in{};
    in.mouse_x = kWidth - 10.0f;
    in.mouse_y = kHeight - 10.0f;
    return in;
}

// 左键按下（Hold，未移动）：按下起点 = (x, 行中心)。
InputSnapshot PressAt(float x) {
    InputSnapshot in = Base();
    in.mouse_x = x;
    in.mouse_y = kRowCy;
    in.left.raw = -2;
    in.left_press_x = x;
    in.left_press_y = kRowCy;
    return in;
}

// 按住并拖到 x（Drag：按下点固定，帧末位置 = x）。
InputSnapshot DragTo(float press_x, float x) {
    InputSnapshot in = Base();
    in.mouse_x = x;
    in.mouse_y = kRowCy;
    in.left.raw = -1;
    in.left_press_x = press_x;
    in.left_press_y = kRowCy;
    return in;
}

// 松开：左键不再按住（raw = 0）。**不是 Click**：拖拽过就不产生 Click。
InputSnapshot Released() { return Base(); }

// 一次键盘点击（单帧）。mods 显式给，避免沿用上一帧的修饰键状态。
InputSnapshot KeyTap(KeyCode key, bool ctrl = false, bool shift = false) {
    InputSnapshot in = Base();
    in.keys[static_cast<int>(key)].raw = 1;
    in.mods.ctrl = ctrl;
    in.mods.shift = shift;
    return in;
}

// 假的系统剪贴板：get 返回固定文本，set 把内容写进外部 string。
struct FakeClipboard {
    std::string text;
    int set_calls = 0;

    static const char* Get(void* userdata) {
        FakeClipboard* self = static_cast<FakeClipboard*>(userdata);
        return self->text.c_str();
    }
    static void Set(const char* text, void* userdata) {
        FakeClipboard* self = static_cast<FakeClipboard*>(userdata);
        self->text = text;
        ++self->set_calls;
    }
};

class TextFixture {
public:
    explicit TextFixture(const char* initial, size_t capacity = 64) {
        std::snprintf(buffer_, capacity, "%s", initial);
        capacity_ = capacity;
        theme_ = SpTheme::Default(kFontSize);
    }

    void EnableFakeClipboard(FakeClipboard* clip) {
        SpUi::SpClipboard cb;
        cb.get = &FakeClipboard::Get;
        cb.set = &FakeClipboard::Set;
        cb.userdata = clip;
        ui_.SetClipboard(cb);
    }

    void Run(const InputSnapshot& in) {
        last_focused_ = false;
        ui_.Begin(in, theme_, kWidth, kHeight, /*frame_dt_ms=*/1000.0f / 30.0f);
        last_focused_ = ui_.InputText("hint", buffer_, capacity_, kBox);
        ui_.End();
        cmds_.Clear();
        ui_.Emit(&cmds_);
    }

    // 连续帧的小工具：按下 → （可选拖动）→ 松开。起止都是**字符边界**。
    void PressAtChar(int index) { Run(PressAt(CharX(index) + 1.0f)); }
    void DragToChar(int press_index, int index) {
        Run(DragTo(CharX(press_index) + 1.0f, CharX(index) + 1.0f));
    }
    void Release() { Run(Released()); }

    // 选中 [begin, end) 后松开（DRAG 需要两帧：按下定位锚点，再拖出选区）。
    void SelectChars(int begin, int end) {
        PressAtChar(begin);
        DragToChar(begin, end);
        Release();
    }

    const char* buffer() const { return buffer_; }
    size_t capacity() const { return capacity_; }
    bool focused() const { return last_focused_; }
    const jpov::RenderCommandList& cmds() const { return cmds_; }

    // 本帧发出的「选区板」数量（alpha 在 (0,1) 的板 → 只能是选区高亮）。
    int SelectionPlateCount() const {
        int n = 0;
        for (const jpov::Plate2DCommand& p : cmds_.plate2d) {
            if (p.fill_color.a > 0.0f && p.fill_color.a < 1.0f) {
                ++n;
            }
        }
        return n;
    }

    // 本帧发出的「光标竖板」数量：宽度 = max(1, border_width)，且不与输入框等宽。
    // 注意只统计与输入框同高的那条（输入框底板宽度远大于 1）。
    int CaretPlateCount() const {
        int n = 0;
        for (const jpov::Plate2DCommand& p : cmds_.plate2d) {
            if (p.size.x() <= 2.0f && p.size.y() >= kBox.size.y() * 0.5f) {
                ++n;
            }
        }
        return n;
    }

private:
    SpUi ui_;               // 必须跨帧持有（焦点/选区/拖拽都在这）
    SpTheme theme_;
    char buffer_[64] = {};
    size_t capacity_ = 64;
    bool last_focused_ = false;
    jpov::RenderCommandList cmds_;
};

// 先让空框/已有文本的框获得焦点（按下 → 松开）。
void Focus(TextFixture* f) {
    f->PressAtChar(0);
    f->Release();
}

// ---- 1. 拖拽选字 + Ctrl+C / X / V（选区内容与剪贴板闭环）----
void CheckDragSelectAndClipboardKeys() {
    TextFixture f("abcdef");
    // 不注入回调：用进程内剪贴板（SpUi 内置退化实现）。
    Focus(&f);
    CHECK_EQ(std::string(f.buffer()), std::string("abcdef"));
    CHECK_EQ(f.CaretPlateCount(), 1) << "聚焦且无选区时应画且只画一条光标";
    CHECK_EQ(f.SelectionPlateCount(), 0) << "无选区时不该画高亮";

    // 选中 "cde"（码点 [2,5)）：按下在第 2 个字符边界，拖到第 5 个。
    f.SelectChars(2, 5);
    CHECK_EQ(std::string(f.buffer()), std::string("abcdef")) << "选择本身不改内容";
    CHECK_EQ(f.SelectionPlateCount(), 1) << "拖拽选择必须画出一块高亮板";
    CHECK_EQ(f.CaretPlateCount(), 0) << "有选区时是“选中态”，不该同时出现单竖杠";

    // Ctrl+C → 剪贴板 = "cde"，buffer 不变。
    f.Run(KeyTap(KeyCode::C, /*ctrl=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string("abcdef"));
    CHECK_EQ(f.SelectionPlateCount(), 1) << "复制不应清掉选区";

    // Ctrl+X → 剪掉 "cde"，光标落到选区头（2）。
    f.Run(KeyTap(KeyCode::X, /*ctrl=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string("abf")) << "Ctrl+X 应删除选区";
    CHECK_EQ(f.SelectionPlateCount(), 0) << "剪切后选区内已不存在，不该还画高亮";

    // Ctrl+V → 粘回 "cde"，光标在 5。
    f.Run(KeyTap(KeyCode::V, /*ctrl=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string("abcdef")) << "Ctrl+V 应把剪贴板插回光标处";

    // 无选区时 Ctrl+C 不应改写剪贴板（set_calls 用外部假剪贴板单独验）。
    FakeClipboard clip;
    clip.text = "外部";
    TextFixture g("xyz");
    g.EnableFakeClipboard(&clip);
    Focus(&g);
    g.Run(KeyTap(KeyCode::C, /*ctrl=*/true));
    CHECK_EQ(clip.text, std::string("外部")) << "无选区时 Ctrl+C 不该覆盖外部剪贴板";
    CHECK_EQ(clip.set_calls, 0);
    LOG(INFO) << "[PASS] 拖拽选区 + Ctrl+C/X/V 闭环（含无选区复制不动剪贴板）";
}

// ---- 2. 选中后：Backspace / Delete / 输入字符都先删选区 ----
void CheckSelectionAwareEditing() {
    {
        TextFixture f("abcdef");
        Focus(&f);
        f.SelectChars(2, 5);                     // "cde"
        f.Run(KeyTap(KeyCode::Backspace));
        CHECK_EQ(std::string(f.buffer()), std::string("abf"))
            << "Backspace 有选区时应删整个选区（不是只删左边一个字）";
    }
    {
        TextFixture f("abcdef");
        Focus(&f);
        f.SelectChars(2, 5);
        f.Run(KeyTap(KeyCode::Delete));
        CHECK_EQ(std::string(f.buffer()), std::string("abf"))
            << "Delete 有选区时应删整个选区";
    }
    {
        TextFixture f("abcdef");
        Focus(&f);
        f.SelectChars(2, 5);
        f.Run(KeyTap(KeyCode::Z, /*ctrl=*/false, /*shift=*/true));
        CHECK_EQ(std::string(f.buffer()), std::string("abZf"))
            << "输入字符应替换选区";
    }
    LOG(INFO) << "[PASS] 选中后 Backspace/Delete/输入都先吃选区";
}

// ---- 3. 方向键把选区折叠成单竖杠（左→选区头，右→选区尾）----
void CheckArrowCollapsesSelection() {
    {
        TextFixture f("abcdef");
        Focus(&f);
        f.SelectChars(2, 5);
        f.Run(KeyTap(KeyCode::Left));
        CHECK_EQ(f.SelectionPlateCount(), 0) << "Left 后应回到单竖杠状态";
        CHECK_EQ(f.CaretPlateCount(), 1) << "Left 后应画出光标";
        f.Run(KeyTap(KeyCode::Backspace));
        CHECK_EQ(std::string(f.buffer()), std::string("acdef"))
            << "Left 应折叠到选区**头**（光标=2）后删掉第 1 个字符 'b'";
    }
    {
        TextFixture f("abcdef");
        Focus(&f);
        f.SelectChars(2, 5);
        f.Run(KeyTap(KeyCode::Right));
        CHECK_EQ(f.SelectionPlateCount(), 0) << "Right 后应回到单竖杠状态";
        f.Run(KeyTap(KeyCode::Z, /*ctrl=*/false, /*shift=*/true));
        CHECK_EQ(std::string(f.buffer()), std::string("abcdeZf"))
            << "Right 应折叠到选区**尾**（光标=5），再插入 Z";
    }
    LOG(INFO) << "[PASS] 方向键折叠选区：左→头 / 右→尾，且不再多走一格";
}

// ---- 4. 框内再按左键取消选中（Danis 明确要求）----
void CheckClickCancelsSelection() {
    TextFixture f("abcdef");
    Focus(&f);
    f.SelectChars(2, 5);
    CHECK_EQ(f.SelectionPlateCount(), 1);

    f.PressAtChar(1);   // 选区外、框内再按一次 → 取消选中并把光标放到 1
    CHECK_EQ(f.SelectionPlateCount(), 0) << "框内再点应取消选中";
    CHECK_EQ(f.CaretPlateCount(), 1);
    f.Release();

    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string("bcdef"))
        << "取消选中后 Backspace 只删光标左边一个字符（='a'），不该再吃旧选区";
    LOG(INFO) << "[PASS] 框内再次点击取消选中";
}

// ---- 5. Ctrl+A 全选 + 输入替换全部 ----
void CheckSelectAll() {
    TextFixture f("abcdef");
    Focus(&f);
    f.Run(KeyTap(KeyCode::A, /*ctrl=*/true));
    CHECK_EQ(f.SelectionPlateCount(), 1) << "Ctrl+A 应画出全选高亮";
    f.Run(KeyTap(KeyCode::Z, /*ctrl=*/false, /*shift=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string("Z")) << "全选后输入应替换整段文本";

    // 空框全选：锚点=光标=0，不应画高亮、也不该崩。
    TextFixture g("");
    Focus(&g);
    g.Run(KeyTap(KeyCode::A, /*ctrl=*/true));
    CHECK_EQ(g.SelectionPlateCount(), 0);
    LOG(INFO) << "[PASS] Ctrl+A 全选 + 覆盖输入（空框退化安全）";
}

// ---- 6. 中文粘贴：码点安全（退格不切半个字）----
void CheckChinesePasteAndCodepointBackspace() {
    TextFixture f("");
    FakeClipboard clip;
    clip.text = "中文abc";
    f.EnableFakeClipboard(&clip);
    Focus(&f);

    f.Run(KeyTap(KeyCode::V, /*ctrl=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string("中文abc"))
        << "Ctrl+V 应原样粘贴中文（UTF-8 多字节不被截断）";

    // 4 次退格：c / b / a / 文 —— 每次都必须整码点删除。
    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string("中文ab"));
    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string("中文a"));
    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string("中文"));
    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string("中"))
        << "退格到中文边界时必须整字删，不能留下半个 UTF-8 序列";

    // Ctrl+A + Backspace：全删。
    f.Run(KeyTap(KeyCode::A, /*ctrl=*/true));
    f.Run(KeyTap(KeyCode::Backspace));
    CHECK_EQ(std::string(f.buffer()), std::string(""));
    LOG(INFO) << "[PASS] 中文粘贴 + 码点安全退格";
}

// ---- 7. 粘贴的单行 sanitize + 容量按整码点截断 ----
void CheckPasteSanitizeAndCapacity() {
    {
        TextFixture f("");
        FakeClipboard clip;
        clip.text = "a\nb\r\nc\td";
        f.EnableFakeClipboard(&clip);
        Focus(&f);
        f.Run(KeyTap(KeyCode::V, /*ctrl=*/true));
        CHECK_EQ(std::string(f.buffer()), std::string("a b c d"))
            << "单行控件应把换行/Tab 变空格（\\r\\n 只出一个空格）";
    }
    {
        // 容量 7 字节：只能放下两个中文（6 字节），第三个放不下 → 整字丢弃。
        TextFixture f("", /*capacity=*/8);
        FakeClipboard clip;
        clip.text = "中文中文";
        f.EnableFakeClipboard(&clip);
        Focus(&f);
        f.Run(KeyTap(KeyCode::V, /*ctrl=*/true));
        CHECK_EQ(std::string(f.buffer()), std::string("中文"))
            << "容量不足时应按整码点截断（写入半个 UTF-8 会得到坏字符）";
        CHECK_LE(std::strlen(f.buffer()), f.capacity() - 1);
    }
    {
        // 非法 UTF-8（独立 continuation 字节）丢弃，绝不把坏字节写进 buffer。
        TextFixture f("");
        FakeClipboard clip;
        clip.text = "a\xE4\xB8\xAD" "b\x80" "c";  // a 中 b <孤立 0x80> c
        f.EnableFakeClipboard(&clip);
        Focus(&f);
        f.Run(KeyTap(KeyCode::V, /*ctrl=*/true));
        CHECK_EQ(std::string(f.buffer()), std::string("a中bc"))
            << "非法 UTF-8 字节应被丢弃，而不是写进 buffer";
    }
    LOG(INFO) << "[PASS] 粘贴 sanitize（换行/Tab/非法字节）+ 整码点截断";
}

// ---- 8. Ctrl 按住时不再插入字母（Ctrl+C 不能顺便打个 'c'）----
void CheckCtrlDoesNotType() {
    TextFixture f("");
    Focus(&f);
    f.Run(KeyTap(KeyCode::C, /*ctrl=*/true));
    CHECK_EQ(std::string(f.buffer()), std::string(""))
        << "Ctrl+C 不应插入 'c'（无选区时 copy 是空操作，buffer 必须保持空）";
    f.Run(KeyTap(KeyCode::A, /*ctrl=*/true));   // 全选（空）
    CHECK_EQ(std::string(f.buffer()), std::string("")) << "Ctrl+A 不应插入 'a'";
    LOG(INFO) << "[PASS] Ctrl 按住时不插入字母";
}

// ---- 9. 焦点跨框隔离：第二个输入框不影响第一个的光标/选区 ----
void CheckTwoInputBoxesIsolation() {
    // 两个框垂直排列；同一 SpUi 面板里先画 A 再画 B。
    const SpRect box_a = {{20.0f, 20.0f}, {200.0f, 30.0f}};
    const SpRect box_b = {{20.0f, 70.0f}, {200.0f, 30.0f}};
    char buf_a[32] = "hello";
    char buf_b[32] = "world";
    SpUi ui;
    SpTheme theme = SpTheme::Default(kFontSize);
    jpov::RenderCommandList cmds;

    auto frame = [&](const InputSnapshot& in) {
        ui.Begin(in, theme, kWidth, kHeight, 1000.0f / 30.0f);
        ui.InputText("a", buf_a, sizeof(buf_a), box_a);
        ui.InputText("b", buf_b, sizeof(buf_b), box_b);
        ui.End();
        cmds.Clear();
        ui.Emit(&cmds);
    };

    // 点 A 的末尾（第 5 个字符）并输入 X。
    InputSnapshot press = Base();
    press.mouse_x = box_a.pos.x() + kPad + kFontSize * 0.6f * 5.0f + 1.0f;
    press.mouse_y = box_a.pos.y() + 15.0f;
    press.left.raw = -2;
    press.left_press_x = press.mouse_x;
    press.left_press_y = press.mouse_y;
    frame(press);
    frame(Released());
    frame(KeyTap(KeyCode::X, /*ctrl=*/false, /*shift=*/true));
    CHECK_EQ(std::string(buf_a), std::string("helloX"));
    CHECK_EQ(std::string(buf_b), std::string("world"));

    // 再聚焦 B（点 B 内部）并输入 Y：A 的文本不能变。
    press.mouse_x = box_b.pos.x() + kPad + kFontSize * 0.6f * 5.0f + 1.0f;
    press.mouse_y = box_b.pos.y() + 15.0f;
    press.left_press_x = press.mouse_x;
    press.left_press_y = press.mouse_y;
    frame(press);
    frame(Released());
    frame(KeyTap(KeyCode::Y, /*ctrl=*/false, /*shift=*/true));
    CHECK_EQ(std::string(buf_a), std::string("helloX")) << "焦点转到 B 后 A 仍被改动";
    CHECK_EQ(std::string(buf_b), std::string("worldY"));
    LOG(INFO) << "[PASS] 双输入框隔离：光标/选区不串框";
}

}  // namespace

int main() {
    CheckDragSelectAndClipboardKeys();
    CheckSelectionAwareEditing();
    CheckArrowCollapsesSelection();
    CheckClickCancelsSelection();
    CheckSelectAll();
    CheckChinesePasteAndCodepointBackspace();
    CheckPasteSanitizeAndCapacity();
    CheckCtrlDoesNotType();
    CheckTwoInputBoxesIsolation();
    LOG(INFO) << "===== sp_ui 输入框选区/剪贴板测试全部通过 =====";
    return 0;
}

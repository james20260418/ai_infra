// JPOV UI S5 自证测试
//
// 验证 S5 文本输入框 InputText（底框 + 光标 + 占位符，焦点 + 键盘写回）：
//   1. 绘制：底框(border 边框 + background 底)；空 buffer 且聚焦时画占位符
//      (disabled 色)；有内容画正文(foreground)；聚焦时在文本末尾画光标(竖线)。
//   2. 焦点：点击 box 内获得焦点(返回 true、画光标)；点击 box 外失焦(返回 false)。
//   3. 键盘写回 char*（限容量）：聚焦时字母/数字/空格写入 buffer；超容量截断；
//      Backspace 删除末字符；Enter/Escape 失焦；修饰键/方向键忽略。
//   4. 超长水平滚动：文本宽度超过 box 时不越出右缘，内部滚动推进。
//   5. 容错：越界/零尺寸 → 0 指令、不聚焦、不改 buffer。
//
// 本测试是纯 CPU 的指令层比对（gold 指令），不渲染、无窗口（headless）。
// 按键通过 InputSnapshot.keys[] 的 KeyState(raw=Click 次数) 模拟。

#include <cstring>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::Color;
using jpov::FillRect2DCommand;
using jpov::InputSnapshot;
using jpov::KeyCode;
using jpov::RenderCommandList;
using jpov::Text2DCommand;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;

// 构造一个无鼠标交互的输入（左键 None）。
InputSnapshot MakePlainInput() {
    InputSnapshot in{};
    in.mouse_x = 100.0f;
    in.mouse_y = 100.0f;
    return in;
}

// 构造一个左键单击（count 次 Click），释放位置在 (cx,cy) 的输入。
InputSnapshot MakeClickInput(float cx, float cy, int count = 1) {
    InputSnapshot in = MakePlainInput();
    in.mouse_x = cx;
    in.mouse_y = cy;
    in.left.raw = static_cast<int8_t>(count);
    for (int i = 0; i < count && i < jpov::kMaxClicksPerFrame; ++i) {
        in.left_clicks[i] = jpov::ClickEvent{cx, cy, 1.0f};
    }
    return in;
}

// 在输入上注册按下某键 n 次（Click 事件）。
void PressKey(InputSnapshot* in, KeyCode key, int n = 1) {
    in->keys[static_cast<int>(key)].raw =
        static_cast<int8_t>(std::min(n, jpov::kMaxClicksPerFrame));
}

// ASCII 可编辑字符（小写字母/数字）→ KeyCode，用于逐帧打字模拟。
KeyCode KeyCodeForAscii(char ch) {
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (ch - 'a'));
    }
    if (ch >= '0' && ch <= '9') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::_0) + (ch - '0'));
    }
    CHECK(ch == ' ') << "KeyCodeForAscii 仅支持 小写字母/数字/空格";
    return KeyCode::Space;
}

// 断言一个 FillRect2D 命令的值与期望一致。
void CheckFill(const FillRect2DCommand& r, float px, float py, float w, float h,
               const Color& fill) {
    CHECK_NEAR(r.pos.x(), px, 0.01f);
    CHECK_NEAR(r.pos.y(), py, 0.01f);
    CHECK_NEAR(r.size.x(), w, 0.01f);
    CHECK_NEAR(r.size.y(), h, 0.01f);
    CHECK_EQ(r.fill_color.r, fill.r);
    CHECK_EQ(r.fill_color.g, fill.g);
    CHECK_EQ(r.fill_color.b, fill.b);
    CHECK_EQ(r.fill_color.a, fill.a);
}

// 判断命令列表里是否存在包含指定子串的文本。
bool HasTextSubstr(const RenderCommandList& cmd, const char* substr) {
    for (const Text2DCommand& t : cmd.text2d) {
        if (t.text.find(substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 返回命令列表里第一条文本命令的颜色（断言列表非空）。
Color FirstTextColor(const RenderCommandList& cmd) {
    CHECK(!cmd.text2d.empty()) << "期望存在文本命令";
    return cmd.text2d[0].color;
}

// 比较两个颜色（Color 无 operator==，逐分量比对）。
void CheckColorEq(const Color& a, const Color& b, const char* what) {
    CHECK_EQ(a.r, b.r) << what;
    CHECK_EQ(a.g, b.g) << what;
    CHECK_EQ(a.b, b.b) << what;
    CHECK_EQ(a.a, b.a) << what;
}

}  // namespace

namespace jpov {

class UiS5Test {
public:
    // 初始态（未聚焦）：不画光标；空 buffer 画占位符（disabled 色，含 label）。
    static void TestInitNotFocused() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        RenderCommandList cmd;
        char buf[64] = "";  // 空 buffer。
        ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
        const bool focused = ui.InputText("name", buf, sizeof(buf), box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!focused) << "初始（未点击）不应聚焦";
        // 底框 1 条 FillRect；无光标 → 总共 1 条 FillRect。
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "应只有底框 1 条 FillRect";
        CheckFill(cmd.fillrect2d[0], 10.0f, 10.0f, 200.0f, 24.0f,
                  theme.background);
        // 空 buffer → 占位符文本（含 label），disabled 色。
        CHECK_EQ(cmd.text2d.size(), 1u) << "空 buffer 应画占位符";
        CHECK(HasTextSubstr(cmd, "name")) << "占位符应含 label";
        CheckColorEq(FirstTextColor(cmd), theme.disabled, "占位符应为 disabled 色");
        LOG(INFO) << "[PASS] InputText 初始未聚焦：底框 + disabled 占位符";
    }

    // 焦点：点击框内 → 聚焦(返回 true、画光标)；点击框外 → 失焦。
    static void TestClickFocusToggle() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        {
            // 点击框内聚焦 → 返回 true，画光标（底框 + 光标 = 2 条 FillRect）。
            Ui ui;
            RenderCommandList cmd;
            char buf[64] = "abc";
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            const bool focused = ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            ui.Emit(&cmd);
            CHECK(focused) << "点击框内应聚焦";
            CHECK_EQ(cmd.fillrect2d.size(), 2u)
                << "聚焦应画 底框+光标 2 条 FillRect";
            CHECK_EQ(cmd.text2d.size(), 1u) << "有内容应画正文";
        }
        {
            // 先聚焦（上一帧点击框内），再点击框外 → 失焦，返回 false。
            Ui ui;
            RenderCommandList cmd;
            char buf[64] = "abc";
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);  // 聚焦。
            ui.End();
            ui.Begin(MakeClickInput(600.0f, 300.0f), theme, 640.0f, 360.0f);
            const bool focused = ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            ui.Emit(&cmd);
            CHECK(!focused) << "点击框外应失焦";
            CHECK_EQ(cmd.fillrect2d.size(), 1u)
                << "失焦后不应画光标（只有底框）";
        }
        LOG(INFO) << "[PASS] InputText 焦点：点击框内聚焦/框外失焦";
    }

    // 键盘写回：聚焦后字母/数字/空格依次写入 buffer；返回 true。
    static void TestKeyboardWritesBuffer() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        char buf[64] = "";
        // 第 1 帧：点击聚焦。
        ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();

        // 第 2 帧起逐帧键入 h e l l o（每帧一健 = 真实 60fps 打字节奏，
        // InputSnapshot 不携带同帧多键的先后顺序，故同帧多键顺序未定义，
        // 测试按逐帧单键验证确定性顺序）。
        for (char ch : {'h', 'e', 'l', 'l', 'o'}) {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCodeForAscii(ch));
            ui.Begin(in, theme, 640.0f, 360.0f);
            const bool focused = ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK(focused) << "聚焦中每帧都应返回 true";
        }
        CHECK_STREQ(buf, "hello") << "键盘应写回 'hello'";
        // 逐帧键入空格 + 数字 1 2 3（每帧一健，避免同帧多键顺序歧义）。
        for (char ch : {' ', '1', '2', '3'}) {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCodeForAscii(ch));
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
        }
        CHECK_STREQ(buf, "hello 123") << "空格与数字应写回";
        LOG(INFO) << "[PASS] InputText 键盘写回 'hello 123' 正确";
    }

    // 限容量：buffer 太小 → 追加到 cap 即截断，不越界、无溢出。
    static void TestCapacityTruncation() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        char buf[5] = "";  // 容量 5 → 最多 4 字符 + '\0'。
        ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();

        InputSnapshot in = MakePlainInput();
        PressKey(&in, KeyCode::A);
        PressKey(&in, KeyCode::B);
        PressKey(&in, KeyCode::C);
        PressKey(&in, KeyCode::D);
        PressKey(&in, KeyCode::E);  // 第 5 个 → 超容量应被截断。
        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();

        CHECK_STREQ(buf, "abcd") << "超过容量应截断为 'abcd'";
        CHECK_EQ(buf[4], '\0') << "末尾必须是终止符，不越界";
        LOG(INFO) << "[PASS] InputText 超容量截断、无溢出";
    }

    // Backspace：删除末字符；空 buffer 时按删除不崩、不变。
    static void TestBackspace() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        char buf[64] = "";
        ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();
        // 输入 "ab"。
        {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::A);
            PressKey(&in, KeyCode::B);
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK_STREQ(buf, "ab");
        }
        // 删一个 → "a"。
        {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::Backspace);
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK_STREQ(buf, "a") << "Backspace 应删末字符";
        }
        // 空再删 → 不变、不崩。
        {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::Backspace, 5);  // 连删 5 次（实际只有 1 个字符）。
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK_STREQ(buf, "") << "空 buffer 再删应保持空";
        }
        // 删除后恢复输入应正确拼接。
        {
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::C);
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK_STREQ(buf, "c") << "删除后追加应正确";
        }
        LOG(INFO) << "[PASS] InputText Backspace 删除/空删安全/删除后拼接";
    }

    // Enter / Escape：失焦（buffer 不变）。
    static void TestEnterEscapeBlurs() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        // Escape。
        {
            Ui ui;
            char buf[64] = "x";
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::Escape);
            ui.Begin(in, theme, 640.0f, 360.0f);
            const bool focused = ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK(!focused) << "Escape 应失焦";
            CHECK_STREQ(buf, "x") << "Escape 不应改 buffer";
            // 下一帧无按键：不再聚焦、不消费键入。
            InputSnapshot in2 = MakePlainInput();
            PressKey(&in2, KeyCode::F);
            ui.Begin(in2, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK_STREQ(buf, "x") << "失焦后键入不应写入";
        }
        // Enter。
        {
            Ui ui;
            char buf[64] = "xy";
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            InputSnapshot in = MakePlainInput();
            PressKey(&in, KeyCode::Enter);
            ui.Begin(in, theme, 640.0f, 360.0f);
            const bool focused = ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            CHECK(!focused) << "Enter 应失焦";
            CHECK_STREQ(buf, "xy") << "Enter 不应改 buffer";
        }
        LOG(INFO) << "[PASS] InputText Enter/Escape 失焦且不改 buffer";
    }

    // 修饰键/方向键忽略：不改 buffer。
    static void TestModifierKeysIgnored() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        char buf[64] = "ab";
        ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();
        InputSnapshot in = MakePlainInput();
        PressKey(&in, KeyCode::LeftShift);
        PressKey(&in, KeyCode::RightShift);
        PressKey(&in, KeyCode::LeftCtrl);
        PressKey(&in, KeyCode::Tab);
        PressKey(&in, KeyCode::Left);
        PressKey(&in, KeyCode::Right);
        PressKey(&in, KeyCode::Up);
        PressKey(&in, KeyCode::F1);
        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();
        CHECK_STREQ(buf, "ab") << "修饰键/方向键/功能键应被忽略";
        LOG(INFO) << "[PASS] InputText 修饰键/方向键忽略";
    }

    // 超长水平滚动：文本宽度超过 box 右缘 → 光标内部滚动、不越出右缘。
    static void TestLongTextScrollDoesNotOverflow() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 窄 box（宽 80px），内容很长 → 触发滚动。
        const UiRect box{{10.0f, 10.0f}, {80.0f, 24.0f}};
        Ui ui;
        char buf[64] = "";
        ui.Begin(MakeClickInput(40.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();

        // 连续多帧打一串很长的字符。
        const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (const char* p = chars; *p; ++p) {
            // 把当前字符的 KeyCode 按下。字母/数字区间：a=A..，0=_0..。
            KeyCode key;
            if (*p >= 'a' && *p <= 'z') {
                key = static_cast<KeyCode>(static_cast<int>(KeyCode::A) +
                                           (*p - 'a'));
            } else {  // 数字。
                key = static_cast<KeyCode>(static_cast<int>(KeyCode::_0) +
                                           (*p - '0'));
            }
            InputSnapshot in = MakePlainInput();
            PressKey(&in, key);
            ui.Begin(in, theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
        }
        CHECK_STREQ(buf, chars) << "长文本应完整写入（未截断）";

        // 最后一帧（无按键）→ 检查光标不越出 box 右缘（S5.3 不溢出）。
        {
            RenderCommandList cmd;
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            ui.Emit(&cmd);
            CHECK(!cmd.fillrect2d.empty()) << "应有底框+光标";
            // 最后一条 = 光标（accent 色、小矩形）。断言其右缘 ≤ box 右缘。
            const FillRect2DCommand& caret = cmd.fillrect2d.back();
            const float right = caret.pos.x() + caret.size.x();
            CHECK_LE(right, box.pos.x() + box.size.x() + 0.01f)
                << "光标不得越出 box 右缘（内部滚动）";
            CHECK_LE(caret.pos.x(), box.pos.x() + box.size.x())
                << "光标起点也不得越界";
        }
        LOG(INFO) << "[PASS] InputText 长文本内部滚动、光标不溢出";
    }

    // gold 指令：聚焦态 = 底框 + 光标 2 条 FillRect，正文 1 条文本。
    static void TestGoldCommands() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        RenderCommandList cmd;
        char buf[64] = "hi";
        ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
        ui.InputText("n", buf, sizeof(buf), box);
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 2u) << "聚焦 = 底框+光标";
        // 底框。
        CheckFill(cmd.fillrect2d[0], 10.0f, 10.0f, 200.0f, 24.0f,
                  theme.background);
        // 光标：accent 色、宽 = max(border,1)=1，高 = 0.7*24=16.8，垂直居中。
        const FillRect2DCommand& caret = cmd.fillrect2d[1];
        CHECK_NEAR(caret.size.x(), 1.0f, 0.01f);
        CHECK_NEAR(caret.size.y(), 16.8f, 0.01f);
        CheckColorEq(caret.fill_color, theme.accent, "光标应为 accent 色");
        // 光标 X = text_left(10+6=16) + len*char_w(2*9.6=19.2) = 35.2。
        //（padding=6，font 16 → char_w=9.6，未滚动）。
        CHECK_NEAR(caret.pos.x(), 16.0f + 2.0f * 9.6f, 0.01f);
        // 正文文本（foreground）。
        CHECK_EQ(cmd.text2d.size(), 1u);
        CHECK(HasTextSubstr(cmd, "hi"));
        CheckColorEq(FirstTextColor(cmd), theme.foreground, "正文应为 foreground 色");
        LOG(INFO) << "[PASS] InputText gold 指令（底框+光标+正文）";
    }

    // 容错：越界/零尺寸 → 0 指令、不聚焦、不改 buffer。
    static void TestOffscreenInputText() {
        const UiTheme theme = UiTheme::Default(16.0f);
        Ui ui;
        RenderCommandList cmd;
        char buf[64] = "keep";
        ui.Begin(MakeClickInput(700.0f, 400.0f), theme, 640.0f, 360.0f);
        const bool c1 = ui.InputText("off", buf, sizeof(buf),
                                     UiRect{{700.0f, 10.0f}, {100.0f, 24.0f}});
        const bool c2 = ui.InputText("zero", buf, sizeof(buf),
                                     UiRect{{10.0f, 10.0f}, {0.0f, 20.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK(!c1 && !c2) << "越界/零尺寸不应聚焦";
        CHECK_STREQ(buf, "keep") << "越界/零尺寸不应改 buffer";
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界/零尺寸不应画指令";
        CHECK_EQ(cmd.text2d.size(), 0u) << "越界/零尺寸不应画文本";
        LOG(INFO) << "[PASS] InputText 越界/零尺寸 → 0 指令、不聚焦、不改 buffer";
    }

    // 文本宽度测量回调（验收 bug#7 根因修复）：设置回调后，光标用回调返回的
    // 真实字体进宽定位，精确贴合文本末尾，而不是脚本内 0.6*font_size/字符 的
    // 等宽估计（对混合 Latin/CJK 会偏宽 1.5~2x，光标漂到文本长度 1.5~2 倍）。
    // 验证：
    //   1. 未设回调 → 光标 X = text_left + len*0.6*font_size（原估计，确定性）。
    //   2. 设回调（返回已知宽度 W）→ 光标 X = text_left + W（真实进宽），
    //      不再是 0.6 估计。
    //   3. 空 buffer（len=0）→ 即使设回调光标也贴文本左缘（无文本不偏移）。
    static void TestCaretUsesTextMeasure() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        char buf[64] = "hello";  // len=5。
        const float text_left = box.pos.x() + theme.padding_px;  // 16

        // 1. 未设回调 → 0.6 估计（len*9.6）。
        {
            Ui ui;
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            ui.Emit(&cmd);
            const FillRect2DCommand& caret = cmd.fillrect2d.back();
            CHECK_NEAR(caret.pos.x(), text_left + 5.0f * 9.6f, 0.01f);
            // 无回调应回退 0.6*font_size/字符 等宽估计。
        }

        // 2. 设回调（返回已知真实宽度 W=40）→ 光标 = text_left + 40。
        {
            // 静态回调：参数按 Ui 注入的签名，返回固定宽度（模拟真实字体进宽）。
            struct Local {
                static float measure(const char* /*text*/, float /*font_size*/,
                                     const char* /*font_alias*/, void* /*ud*/) {
                    return 40.0f;  // 固定真实宽度（“hello”等 ASCII 在 CJK 的字宽）。
                }
            };
            Ui ui;
            ui.SetTextMeasure(&Local::measure);
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", buf, sizeof(buf), box);
            ui.End();
            ui.Emit(&cmd);
            const FillRect2DCommand& caret = cmd.fillrect2d.back();
            CHECK_NEAR(caret.pos.x(), text_left + 40.0f, 0.01f);
            // 设回调后光标用真实进宽(text_left + 40)，非 0.6 估计。
        }

        // 3. 空 buffer（len=0）+ 设回调 → 光标仍贴文本左缘（无文本不偏移）。
        {
            char empty[8] = "";
            struct Local {
                static float measure(const char* /*text*/, float /*font_size*/,
                                     const char* /*font_alias*/, void* /*ud*/) {
                    return 999.0f;  // 若误用会明显偏移，但 len=0 不应触发测量。
                }
            };
            Ui ui;
            ui.SetTextMeasure(&Local::measure);
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(50.0f, 20.0f), theme, 640.0f, 360.0f);
            ui.InputText("n", empty, sizeof(empty), box);
            ui.End();
            ui.Emit(&cmd);
            const FillRect2DCommand& caret = cmd.fillrect2d.back();
            CHECK_NEAR(caret.pos.x(), text_left, 0.01f);
            // 空 buffer 光标贴文本左缘（不因回调偏移）。
        }
        LOG(INFO) << "[PASS] InputText 光标用文本测量回调精确定位(验收 bug#7)";
    }

    static void RunAll() {
        TestInitNotFocused();
        TestClickFocusToggle();
        TestKeyboardWritesBuffer();
        TestCapacityTruncation();
        TestBackspace();
        TestEnterEscapeBlurs();
        TestModifierKeysIgnored();
        TestLongTextScrollDoesNotOverflow();
        TestGoldCommands();
        TestOffscreenInputText();
        TestCaretUsesTextMeasure();
        LOG(INFO) << "===== UI S5 (InputText) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s5_test");
    jpov::UiS5Test::RunAll();
    return 0;
}

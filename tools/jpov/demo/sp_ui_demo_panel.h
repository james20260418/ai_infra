// JPOV UI 集成 demo — 调试台面板（共享布局）
//
// 定位：S7 集成 demo 的唯一布局真相。**同时**被两处消费，保证不产生
// generator/test 分叉（吸取 mrquad 参数分叉教训，见 memory/project-jpov.md）：
//   1. jpov_ui_demo（交互窗口版）：OneIteration 每帧调用 DrawSpUIDemoPanel，
//      渲染全部控件 + 实时 Log 框，Danis 可操作性验收。
//   2. jpov_ui_s7_test（纯 CPU 自证）：同一函数、同一状态、空输入，一次 Emit，
//      对产出的整段 2D 指令做 gold 全貌比对。
//
// 面板内容（1280x720 全屏调试台）：
//   - 标题 + 全局控件区：滑条(gain)、复选框(enable)、Combo(profile)、
//     Combo(view_mode，与 profile 同排并排 = 同屏两个下拉)、
//     输入框(name)、色块(color swatch)、按钮行(重置/应用)
//   - 函数包装复用区：for 循环画 2~3 份同一电机子面板（多电机），各自独立 state
//   - 实时 Log 输出框：把每次交互事件（按钮/滑条/勾选/下拉/输入）以时间戳
//     文本实时追加显示，供验收者确认每个动作都被正确捕获。
//
// 布局约定：所有 SpRect 均为 SpUi 面板局部坐标（像素）。root 面板位于窗口原点
// (0,0)，故本面板坐标即窗口绝对坐标。渲染分辨率 = 窗口分辨率 = 1280x720，
// 面板与窗口 1:1 对应，无缩放歧义。
//
// 状态外置：全部控件状态在 SpDemoState 中（符合"无状态渲染 + 显式状态"约定），
// SpUi 对象跨帧持有，本函数每帧从零重建指令，交互写回 state。
//
// 任何一处控件/布局改动，交互 demo 与自证测试都会同步感知（同一份代码），
// 从根本上杜绝 golden 与用户实际看到的差异。

#ifndef JPOV_SP_UI_DEMO_PANEL_H_
#define JPOV_SP_UI_DEMO_PANEL_H_

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/sp_ui/sp_ui.h"

namespace jpov::sp_ui {

// ============================================================================
// 实时 Log 输出框（环形缓冲，newest-first 滚动显示）
// ============================================================================

// 一个实时 log 条目：时间戳前缀 + 文本。
struct SpDemoLogEntry {
    char text[96];
};

// 实时 Log 输出框。跨帧持有（SpDemoState 内），每帧把可见条目画进 log 区。
// Append 追加新条目；Draw 时最新在最上、向下滚动（类似控制台）。
// 环形缓冲存储最近 kCapacity 条，避免无界增长。
class SpDemoLog {
public:
    static constexpr int kCapacity = 64;   // 环形缓冲容量
    static constexpr int kVisible = 10;    // 一屏最多显示条数

    // 追加一条带时间戳的 log 条目（printf 风格）。
    // timestamp_s 由调用方给（交互 demo = 累计秒；自证测试 = 伪秒），
    // 保证时序可确定性比对。
    void Append(double timestamp_s, const char* fmt, ...) {
        char body[sizeof(SpDemoLogEntry::text) - 16];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        SpDemoLogEntry e;
        snprintf(e.text, sizeof(e.text), "[%.1fs] %s", timestamp_s, body);
        if (count_ < kCapacity) {
            buf_[count_++] = e;
        } else {
            buf_[head_ % kCapacity] = e;
            ++head_;
        }
    }

    // 清空（重置可调用）。
    void Clear() {
        count_ = 0;
        head_ = 0;
    }

    int Count() const { return count_; }

    // 第 i 条（i=0 最旧，i=Count()-1 最新）。Draw 时从最新往回取 kVisible。
    const SpDemoLogEntry& At(int i) const {
        CHECK_GE(i, 0) << "log index must be >= 0";
        CHECK_LT(i, count_) << "log index out of range: " << i;
        return buf_[(head_ + i) % kCapacity];
    }

private:
    SpDemoLogEntry buf_[kCapacity];
    int count_ = 0;
    int head_ = 0;  // 写指针（% kCapacity）
};

// ============================================================================
// 子面板状态（函数包装复用演示：多电机，各自独立 state）
// ============================================================================

// 单个电机子面板的状态（函数包装复用：2~3 份同一子面板、各自独立 state）。
struct UiDemoMotor {
    float speed = 0.0f;     // 速度滑条 [0, 100]
    bool inverted = false;  // 反转复选框
};

// ============================================================================
// 整个调试台的跨帧状态（状态外置，全部控件写回这里）
// ============================================================================

struct SpDemoState {
    // ---- 全局控件 ----
    float gain = 5.0f;         // 增益滑条 [0, 10]
    bool enable = true;        // 使能复选框
    // 材质 flipbook 的时间驱动开关（true = 翻书走，false = 定格在当前帧）。    // 时间本身仍由调用方给（SpUi::Begin 的 time_seconds）—— 这个开关只决定
    // 调用方要不要推进那个秒数，引擎侧依然不取自己的时钟。
    bool anim_play = true;
    // 材质水波方式：0 = 关键帧溶解（旧）/ 1 = 正弦波 / 2 = 涟漪 / 3 = 噪声流。
    // 四种图集都是 4x4 / 16 帧，只是生成方式不同（见 assets/sp_ui/marble/README.md）。
    int wave_mode = 1;
    int profile = 1;           // 运行模式 Combo 选中 index
    int view_mode = 0;         // 显示模式 Combo 选中 index（第二个下拉，同屏）
    char name[32] = "";        // 电机命名输入框
    // ---- 多电机（函数包装复用）----
    static constexpr int kMotorCount = 3;  // 2~3 份同一子面板（S7.3）
    UiDemoMotor motors[kMotorCount];
    // ---- 实时 Log + 帧时钟 ----
    SpDemoLog log;
    int frame = 0;     // 帧计数
    double time_s = 0.0;  // 自启动秒数（log 时间戳源）
};

// ============================================================================
// 布局常量（1280x720 面板；所有值 = 像素，SpUi 面板局部坐标 = 窗口坐标）
// ============================================================================

// 面板尺寸（= 渲染/窗口分辨率，1:1）。
inline constexpr float kSpUiDemoWidth = 1280.0f;
inline constexpr float kSpUiDemoHeight = 720.0f;

// 主题字号：面板 16px；Log 框 14px（容纳更多行）。
inline constexpr float kSpUiDemoFontSize = 16.0f;
inline constexpr float kUiDemoLogFontSize = 14.0f;

// 布局系数。
inline constexpr float kUiDemoPad = 16.0f;      // 边距
inline constexpr float kUiDemoControlH = 26.0f; // 控件行高
inline constexpr float kUiDemoSectionH = 24.0f; // 区块标题行高

// 左列（控件）/ 右列（Log）。
inline constexpr float kUiDemoLeftW = 544.0f;
inline constexpr float kUiDemoRightX = 620.0f;
inline constexpr float kUiDemoRightW =
    kSpUiDemoWidth - kUiDemoRightX - kUiDemoPad;  // = 644
inline constexpr float kUiDemoRightTop = 84.0f;
inline constexpr float kUiDemoRightBottom = kSpUiDemoHeight - kUiDemoPad;

// Y 布局。
inline constexpr float kUiDemoTitleY = 12.0f;
inline constexpr float kUiDemoGlobalHeaderY = 52.0f;
inline constexpr float kUiDemoGlobalY = 84.0f;   // 全局控件首行
inline constexpr float kUiDemoMotorHeaderY = 292.0f;
inline constexpr float kUiDemoLogHeaderY = 52.0f;

// 多电机卡片：横向 3 份并排（函数包装复用）。
inline constexpr float kUiDemoMotorCardY = 320.0f;
inline constexpr float kUiDemoMotorCardH = 120.0f;
inline constexpr float kUiDemoMotorCardGap = 12.0f;
inline constexpr float kUiDemoMotorCardW =
    (kUiDemoLeftW - 2.0f * kUiDemoMotorCardGap) / 3.0f;  // ≈ 173

// ============================================================================
// 布局辅助（在面板绘制之前定义，供其使用）
// ============================================================================

// 给定某 Y 坐标的一行控件（默认左列宽、控件高）。
inline SpRect RowAt(float y, float width = kUiDemoLeftW) {
    return SpRect{{kUiDemoPad, y}, {width, kUiDemoControlH}};
}

// 把"全局控件第 row 行（0 起）"映射到一个横铺 box（行距 34px）。
inline SpRect GlobalRow(int row, float width = kUiDemoLeftW) {
    return SpRect{{kUiDemoPad,
                   kUiDemoGlobalY + static_cast<float>(row) * 34.0f},
                  {width, kUiDemoControlH}};
}

// 面板内两个下拉（Combo）的 box（面板局部坐标）。
// 【对外导出】：绘制与自证测试合成点击位置共用同一份布局，杜绝分叉
// （否则测试自造坐标会与面板实际位置漂移，断言形式上一旦错就形同虚设）。
// 两者同一排并排 → 同屏两个下拉（多下拉互相干扰的复现/回归场景）。
inline SpRect UiDemoProfileComboBox() {
    return GlobalRow(2, 240.0f);  // 「运行模式」左
}
inline SpRect UiDemoViewModeComboBox() {
    // 与其同排并排：右移「一框宽 + 16px 间距」（=272）——不写魔法数 256，
    // 改 profile 框宽时这里自动跟随。
    const SpRect profile = UiDemoProfileComboBox();
    return SpRect{{profile.pos.x() + profile.size.x() + 16.0f, profile.pos.y()},
                  {240.0f, kUiDemoControlH}};  // 「显示模式」右（同排）
}

// ============================================================================
// 子面板绘制：单个电机卡片（函数包装复用）
// ============================================================================

// 绘制一个电机卡片（底色 + 标题 + 速度滑条 + 反转复选框），交互写回 st 并写 log。
// 独立成函数、for 循环每帧传不同 i → 复用演示（S7.3）。
// 卡片底是静态装饰矩形（直接画到 cmd），控件仍走 ui（命中/状态）。
inline void DrawMotorCard(SpUi& ui, RenderCommandList* cmd, SpDemoState& st, int i,
                          const SpRect& box) {
    CHECK_GE(i, 0);
    CHECK_LT(i, SpDemoState::kMotorCount);
    CHECK(cmd != nullptr);
    UiDemoMotor* m = &st.motors[i];
    char title[32];
    snprintf(title, sizeof(title), "电机 %d", i + 1);

    // 卡片底（静态矩形，全宽；区分 3 份）。
    // sp_ui：走 `ui.Plate` 而不是 cmd->DrawFillRect —— 装饰底也吃主题的材质/剖面（同源）。
    // 颜色取自主题（不硬编码），否则换主题只会换一半。
    ui.Plate(SpRect{box.pos, box.size}, ui.theme().background, 4.0f);
    // 标题。
    ui.Text(title,
            SpRect{{box.pos.x() + kUiDemoPad, box.pos.y() + 4.0f},
                   {box.size.x() - 2.0f * kUiDemoPad, 22.0f}});
    // 速度滑条（label="speed"）。
    if (ui.SliderFloat("speed", &m->speed,
                       SpRect{{box.pos.x() + kUiDemoPad, box.pos.y() + 34.0f},
                              {box.size.x() - 2.0f * kUiDemoPad,
                               kUiDemoControlH}},
                       0.0f, 100.0f)) {
        st.log.Append(st.time_s, "电机%d 速度→%.0f", i + 1, m->speed);
    }
    // 反转复选框。
    if (ui.Checkbox("反转", &m->inverted,
                    SpRect{{box.pos.x() + kUiDemoPad, box.pos.y() + 70.0f},
                           {110.0f, kUiDemoControlH}})) {
        st.log.Append(st.time_s, "电机%d 反转→%s", i + 1,
                      m->inverted ? "开" : "关");
    }
}

// 排布并绘制 kMotorCount 份电机子面板（for 循环 + 同一函数 = 复用演示）。
inline void DrawMotors(SpUi& ui, RenderCommandList* cmd, SpDemoState& st) {
    for (int i = 0; i < SpDemoState::kMotorCount; ++i) {
        const float x = kUiDemoPad + static_cast<float>(i) *
            (kUiDemoMotorCardW + kUiDemoMotorCardGap);
        DrawMotorCard(ui, cmd, st, i,
                      SpRect{{x, kUiDemoMotorCardY},
                             {kUiDemoMotorCardW, kUiDemoMotorCardH}});
    }
}

// ============================================================================
// 整个调试台绘制（唯一入口；交互 demo 与自证测试共用）
// ============================================================================
//
// 每帧调用：填充全部控件 + 处理交互 + 写 Log + 画 Log 框（含静态背景）。
// 静态背景（电机卡片底、Log 底框）是声明式装饰矩形，走 `SpUi::Plate()`
// （同一套材质/剖面，但不属于交互控件、不参与命中）；控件走 ui，交互命中/状态写回。
// 不 Emit；由调用方负责 ui.End() + ui.Emit()（本函数已把背景写进 cmd）。
inline void DrawSpUIDemoPanel(SpUi& ui, RenderCommandList* cmd, SpDemoState& st) {
    CHECK(cmd != nullptr);

    // ---- 静态背景（先画，控件后画叠在上层）----
    // 全窗口底板：整套界面坐在这块板材上（旧 ui 是窗口 clear 色 + 零散矩形，
    // 没有「面板本体」）。sp_ui 加这一层，主题换材质时整块面板一起变。
    // Danis 2026-10-27：背景也要材质（不要平色）。平铺重复靠 plate_material_px
    // 控制密度 —— 背景面积大，格子数自然多，纹路尺度不会跟着被拉大。
    ui.Plate(SpRect{{0.0f, 0.0f}, {kSpUiDemoWidth, kSpUiDemoHeight}},
             ui.theme().background, 0.0f);

    // 实时 Log 底框（右列矩形）。颜色取主题（不硬编码，换主题不再只换一半）。
    ui.Plate(SpRect{{kUiDemoRightX, kUiDemoRightTop},
                    {kUiDemoRightW, kUiDemoRightBottom - kUiDemoRightTop}},
             ui.theme().background, 4.0f);

    // ---- 标题 ----
    ui.Text("JPOV 即时模式 UI 调试台",
            SpRect{{kUiDemoPad, kUiDemoTitleY}, {kUiDemoLeftW, 26.0f}});

    // ---- 全局控件区 ----
    ui.Text("全局参数",
            SpRect{{kUiDemoPad, kUiDemoGlobalHeaderY}, {200.0f, kUiDemoSectionH}});

    // 增益滑条 [0,10]（decimal_places=1 展示小数位显示）。
    if (ui.SliderFloat("增益", &st.gain, GlobalRow(0), 0.0f, 10.0f,
                       /*decimal_places=*/1)) {
        st.log.Append(st.time_s, "增益调整为 %.1f", st.gain);
    }

    // 使能复选框。
    if (ui.Checkbox("使能 Enable", &st.enable, GlobalRow(1, 160.0f))) {
        st.log.Append(st.time_s, "使能切换为 %s", st.enable ? "开" : "关");
    }

    // 材质翻书的时间驱动开关（与使能同排，靠右）。
    // 不复用 GlobalRow —— 那从左边距 16 起刻，这里要排在「使能」右边。
    const SpRect anim_box{{
        kUiDemoPad + 168.0f, kUiDemoGlobalY + 34.0f}, {190.0f, 26.0f}};
    if (ui.Checkbox("时间驱动 Anim", &st.anim_play, anim_box)) {
        st.log.Append(st.time_s, "时间驱动→%s", st.anim_play ? "走" : "停");
    }

    // 运行模式 Combo。
    {
        static const std::vector<const char*> kProfiles = {"标准", "高速", "节能"};
        const int before = st.profile;
        SpRect box = UiDemoProfileComboBox();
        if (ui.Combo("运行模式", &st.profile, kProfiles, box) &&
            before != st.profile) {
            st.log.Append(st.time_s, "运行模式→%s", kProfiles[st.profile]);
        }
    }

    // 显示模式 Combo：第二个下拉，与「运行模式」同排并排（同屏两个下拉）。
    // 独立 state（view_mode），用于复现/验收"同屏多个下拉互相干扰"的问题。
    {
        static const std::vector<const char*> kViewModes = {"实体", "线框",
                                                           "法线"};
        const int before = st.view_mode;
        if (ui.Combo("显示模式", &st.view_mode, kViewModes,
                     UiDemoViewModeComboBox()) &&
            before != st.view_mode) {
            st.log.Append(st.time_s, "显示模式→%s", kViewModes[st.view_mode]);
        }
    }

    // 电机命名输入框。
    {
        SpRect box = GlobalRow(3, 360.0f);
        ui.InputText("输入电机名…", st.name, sizeof(st.name), box);
    }

    // 材质水波方式（3 选 1：正弦 / 涟漪 / 噪声，外加旧的溶解做法做对照）。
    // 放在输入框右侧的空位，不占新行。
    {
        static const std::vector<const char*> kWaveModes = {
            "溶解(旧)", "正弦波", "涟漪", "噪声流"};
        const SpRect wave_box{{
            kUiDemoPad + 372.0f, kUiDemoGlobalY + 3.0f * 34.0f},
            {kUiDemoLeftW - 372.0f, kUiDemoControlH}};
        const int before = st.wave_mode;
        if (ui.Combo("水波", &st.wave_mode, kWaveModes, wave_box) &&
            before != st.wave_mode) {
            st.log.Append(st.time_s, "水波→%s", kWaveModes[st.wave_mode]);
        }
    }

    // 色块（不可编辑，纯展示）+ 标注。
    ui.Text("色样", RowAt(kUiDemoGlobalY + 136.0f, 60.0f));
    ui.ColorSwatch("", {0.25f, 0.60f, 0.95f, 1.0f},
                   SpRect{{kUiDemoPad + 64.0f, kUiDemoGlobalY + 136.0f},
                          {40.0f, 40.0f}});

    // 按钮行：重置 / 应用。
    if (ui.Button("重置",
                  SpRect{{kUiDemoPad, kUiDemoGlobalY + 190.0f}, {120.0f, 30.0f}})) {
        st.log.Append(st.time_s, "点击【重置】");
    }
    if (ui.Button("应用",
                  SpRect{{kUiDemoPad + 136.0f, kUiDemoGlobalY + 190.0f},
                         {120.0f, 30.0f}})) {
        st.log.Append(st.time_s, "点击【应用】");
    }

    // ---- 多电机区（函数包装复用）----
    ui.Text("电机组（同一子面板 ×3）",
            SpRect{{kUiDemoPad, kUiDemoMotorHeaderY}, {360.0f, kUiDemoSectionH}});
    DrawMotors(ui, cmd, st);

    // ---- 实时 Log 输出框 ----
    ui.Text("实时日志",
            SpRect{{kUiDemoRightX, kUiDemoLogHeaderY}, {200.0f, kUiDemoSectionH}});
    // Log 底框背景已在函数头部画过（静态矩形）。
    // 逐条画可见 log（newest-first，最新在最上）。
    const int total = st.log.Count();
    const int n = std::min(total, SpDemoLog::kVisible);
    const float line_h = kUiDemoLogFontSize + 6.0f;  // log 行高（字号+间距）
    if (n > 0) {
        const float char_w = kUiDemoLogFontSize * 0.6f;  // 等宽近似（同 S5）
        for (int k = 0; k < n; ++k) {
            const int idx = total - 1 - k;  // k=0 → 最新一条
            const float line_y =
                kUiDemoRightTop + 8.0f + static_cast<float>(k) * line_h;
            const char* s = st.log.At(idx).text;
            // 左对齐：给一个按内容宽度估算的 box → Text(居中) 后左缘恰在 box 左缘。
            const float text_w = static_cast<float>(std::strlen(s)) * char_w;
            const SpRect line_box{
                {kUiDemoRightX + 8.0f, line_y},
                {text_w, kUiDemoControlH - 6.0f},
            };
            ui.Text(s, line_box);
        }
    }
}

}  // namespace jpov::sp_ui

#endif  // JPOV_SP_UI_DEMO_PANEL_H_

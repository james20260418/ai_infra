// JPOV UI 集成 demo — 交互调试台（S7）
//
// 运行一个 1280x720 的交互窗口，展示完整即时模式 UI 调试台：
//   所有控件（滑条/复选框/Combo/输入框/色块/按钮） + 实时 Log 输出框
//   + 函数包装复用（3 份电机子面板，各自独立 state）。
//
// 布局本身定义在 <ui_demo_panel.h>（DrawUiDemoPanel），交互 demo 与
// 自证测试共用同一份布局，杜绝两种视角分叉。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   bazel run //tools/jpov:jpov_ui_demo
//
// 双平台产物（仿 build_jpov_demo.sh）：
//   ./tools/jpov/build_jpov_ui_demo.sh
//   → output/jpov_ui_demo/{jpov_ui_demo(jpov_ui_demo.exe)}

#include <cstdio>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/demo/ui_demo_panel.h"

// 本 demo 用内置 CJK 字体渲染文本（JPOV 默认加载，alias="CJK"=
// NotoSansCJK），以正确显示中文（标题/全局参数/电机卡等含中文）。
// Ui 发出的文本指令 font_alias 为空（纯 CPU 指令层 golden 不关心字体，
// 见 ui.h / ui.cc）；交互渲染层需要真实可用的字体别名，这里在 Emit 后
// 把空别名统一替换为内置 CJK 字体，使窗口能实际画出文字（含中文）。
static const char* const kDemoFontAlias = jpov::kFontBuiltinCJK;

class UiDemoApp : public JPOV {
public:
    using JPOV::JPOV;

    // 把 Ui 的文本测量回调接到本应用的 JPOV::MeasureTextWidth（真实字体进宽），
    // 使 InputText 光标精确贴合文本末尾，不再用 0.6em 等宽估计（那会使混合
    // Latin/CJK 时光标漂到 1.5~2x 文本长度——验收 bug#7）。
    // userdata = UiDemoApp*，转发到 JPOV::MeasureTextWidth（alias 空串 =
    // 首个注册字体，与 demo 文本渲染用的第一个内置字体一致）。
    static float DemoTextWidth(const char* text, float font_size,
                               const char* /*font_alias*/, void* userdata) {
        UiDemoApp* app = static_cast<UiDemoApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 供 main 在 Init() 后安装文本测量回调（ui_ 私有，经此公开入口设置）。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&UiDemoApp::DemoTextWidth, this);
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)winfo;

        // 渲染分辨率 = 窗口分辨率 1280x720（面板与窗口 1:1，见 ui_demo_panel.h）。
        cmds->camera.fbo_3d_width_ = jpov::kUiDemoWidth;
        cmds->camera.fbo_3d_height_ = jpov::kUiDemoHeight;

        // 主题（16px flat 深色）。
        const jpov::UiTheme theme = jpov::UiTheme::Default(jpov::kUiDemoFontSize);

        // 帧时钟（log 时间戳源 + 键盘 hold 150ms 阈值计时）：固定 30fps 帧周期
        // （与 target_fps 一致）。frame_dt_ms 传给 Ui::Begin 供 hold 重复计时。
        st_.frame = static_cast<int>(frame_count);
        st_.time_s = static_cast<double>(frame_count) * (1.0 / 30.0);
        const float frame_dt_ms = 1000.0f / 30.0f;  // ≈33.33ms/帧

        // 即时模式：每帧 Begin → 画整台 → End → Emit。
        ui_.Begin(input, theme, jpov::kUiDemoWidth, jpov::kUiDemoHeight,
                  frame_dt_ms);
        jpov::DrawUiDemoPanel(ui_, cmds, st_);
        ui_.End();
        ui_.Emit(cmds);

        // 把空字体别名替换为内置 CJK（仅在窗口渲染层需要；gold 测试走纯
        // CPU 指令比对不依赖字体）。只改本帧已 emit 的文本命令。
        for (jpov::Text2DCommand& t : cmds->text2d) {
            if (t.font_alias.empty()) {
                t.font_alias = kDemoFontAlias;
            }
        }
    }

private:
    jpov::Ui ui_;           // 跨帧持有（InputText 焦点 / Combo 展开态内部记忆）。
    jpov::UiDemoState st_;  // 跨帧持有（全部控件状态 + 实时 log + 帧时钟）。
};

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV — 即时模式 UI 调试台 (S7)";
    cfg.width = static_cast<int>(jpov::kUiDemoWidth);
    cfg.height = static_cast<int>(jpov::kUiDemoHeight);
    cfg.target_fps = 30;
    // fonts 留空 → 加载内置默认字体（Latin+CJK），见 kDemoFontAlias（用 CJK 显中文）。
    UiDemoApp app(cfg);
    app.Init();
    // 注入真实字体文本宽度测量 → InputText 光标贴合文本末尾（修复验收 bug#7）。
    app.InstallTextMeasure();
    app.Run();
    app.Finalize();
    return 0;
}

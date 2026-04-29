#ifndef JPOV_JPOV_H_
#define JPOV_JPOV_H_

#include <cstdint>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/window_info.h"

// ============================================================================
// JPOV — 轻型渲染窗口框架
//
// 用法:
//   class MyApp : public JPOV {
//       void OneIteration(int64_t frame_count,
//                         const InputSnapshot& input,
//                         const WindowInfo& winfo,
//                         RenderCommandList& cmds) override {
//           // 画东西...
//       }
//   };
//
//   int main() {
//       MyApp app;
//       app.Run();
//   }
// ============================================================================
class JPOV {
public:
    struct Config {
        const char* title        = "JPOV";
        int         width        = 1280;
        int         height       = 720;
        bool        resizable    = true;
        bool        fullscreen   = false;
        bool        show_console = false;  // Windows 下是否显示命令行窗口
    };

    explicit JPOV(Config cfg);
    virtual ~JPOV();

    // 用户实现的每帧渲染逻辑
    //
    // frame_count — 从 0 开始的帧计数器，单调递增
    // input       — 本帧输入快照（鼠标/键盘状态）
    // winfo       — 本帧窗口尺寸信息
    // cmds        — 输出：填充渲染指令列表，帧末由框架消费
    //
    // Pre-condition: frame_count >= 0
    virtual void OneIteration(int64_t frame_count,
                              const jpov::InputSnapshot& input,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList& cmds) = 0;

    // 创建窗口并进入事件循环，阻塞到退出
    void Run();

private:
    Config config_;

    // ---- 帧循环子步骤（当前 PR 只声明，不实现） ----

    // 采集本帧输入（鼠标/键盘/窗口事件）
    // Pre-condition: 窗口已经创建且有效
    void CaptureInput();

    // 消费渲染指令列表，提交绘制到当前帧
    // Pre-condition: cmds 已由 OneIteration 填充
    void RenderCommands(const jpov::RenderCommandList& cmds);
};

#endif  // JPOV_JPOV_H_

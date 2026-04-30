#ifndef JPOV_JPOV_H_
#define JPOV_JPOV_H_

#include <cstdint>
#include <GLFW/glfw3.h>

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
//                         RenderCommandList* cmds) override {
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

        // OneIteration 调用帧率（Hz），默认 60。
        // Run() 按此帧率调度 OneIteration，实际帧率受 vsync 限制。
        // 设为 0 表示不限制（尽可能快）。
        int target_fps = 60;
    };

    explicit JPOV(Config cfg);
    virtual ~JPOV();

    // 用户实现的每帧渲染逻辑
    //
    // frame_count — 从 0 开始的帧计数器，单调递增
    // input       — 本帧输入快照（鼠标/键盘状态）
    // winfo       — 本帧窗口尺寸信息
    // cmds        — 输出：填充渲染指令列表，帧末由框架消费。非空指针。
    //
    // Pre-condition: frame_count >= 0
    // Pre-condition: cmds != nullptr
    virtual void OneIteration(int64_t frame_count,
                              const jpov::InputSnapshot& input,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds) = 0;

    // 创建窗口并进入事件循环，阻塞到退出
    void Run();

private:
    // ---- 鼠标按键跨帧状态 ----
    struct MouseButtonState {
        // 鼠标当前是否处于按下状态
        bool is_down = false;

        // 本帧内是否收到过 GLFW_RELEASE
        // Drag 判定条件：is_down && !released_this_frame
        bool released_this_frame = false;

        // 最近一次 GLFW_PRESS 的时刻（glfwGetTime 值，秒）
        double press_time = 0.0;
    };

    // ---- 帧内累计事件（CaptureInput 每帧末结算） ----
    struct FrameEvents {
        int left_clicks   = 0;
        int right_clicks  = 0;
        int middle_clicks = 0;

        // ClickEvent 暂存区：HandleMouseButton 释放时填入
        // CaptureInput 结算时写入 InputSnapshot 对应数组
        jpov::ClickEvent left_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent right_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent middle_clicks_detail[jpov::kMaxClicksPerFrame];
    };

    // ---- 鼠标跟踪 ----
    double mouse_x_       = 0.0;
    double mouse_y_       = 0.0;
    double mouse_last_x_  = 0.0;
    double mouse_last_y_  = 0.0;
    double scroll_delta_  = 0.0;

    MouseButtonState left_btn_;
    MouseButtonState right_btn_;
    MouseButtonState middle_btn_;
    FrameEvents frame_;
    double frame_start_time_ = 0.0;  // 当前帧开始时刻（glfwGetTime）
    Config config_;
    GLFWwindow* window_ = nullptr;

    // ---- GLFW 回调（静态转发） ----
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods);
    static void OnMouseMove(GLFWwindow* window, double xpos, double ypos);
    static void OnScroll(GLFWwindow* window, double xoffset, double yoffset);
    static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods);

    // 回调内部转发目标（通过 glfwGetWindowUserPointer 获取 this）
    void HandleMouseButton(int button, int action, double now);
    void HandleMouseMove(double xpos, double ypos);
    void HandleScroll(double yoffset);
    void HandleKey(int key, int scancode, int action, int mods);

    // ---- 帧循环子步骤 ----

    // 采集本帧输入（鼠标/键盘/窗口事件），填充 InputSnapshot
    // Pre-condition: window_ 非空有效
    // Post-condition: input 已由 GLFW 回调数据填充
    void CaptureInput(jpov::InputSnapshot* input);

    // 消费渲染指令列表，提交绘制到当前帧
    // Pre-condition: cmds 已由 OneIteration 填充
    void RenderCommands(const jpov::RenderCommandList& cmds);

    // 帧间隔（秒）：target_fps > 0 则返回 1/target_fps，否则返回 1/60
    double FrameInterval() const;

    // 将一个鼠标键的帧内事件结算到 InputSnapshot
    static void FlushMouseButton(jpov::MouseState* out,
                                 jpov::ClickEvent* out_clicks,
                                 const MouseButtonState& btn,
                                 int click_count,
                                 const jpov::ClickEvent* click_detail);

    // Click 超时阈值（秒）
    static constexpr double kClickDelta = 0.3;

};

#endif  // JPOV_JPOV_H_

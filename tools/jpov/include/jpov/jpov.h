// JPOV — 轻型渲染窗口框架
//
// 用法：
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
//
// JPOV = 用户交互 + 运行调度 + 渲染（通过 Renderer）
// Renderer 是内部 GL 复杂度消化器，用户不可见。

#ifndef JPOV_JPOV_H_
#define JPOV_JPOV_H_

#include <cstdint>
#include <memory>
#include <GLFW/glfw3.h>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/window_info.h"

// Renderer 前向声明
namespace jpov {
class Renderer;
}

// ============================================================================
// JPOV — 轻型渲染窗口框架
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
    // cmds        — 填充渲染指令列表，帧末由框架消费。非空指针。
    //
    // Pre-condition: frame_count >= 0
    // Pre-condition: cmds != nullptr
    virtual void OneIteration(int64_t frame_count,
                              const jpov::InputSnapshot& input,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds /*output*/) = 0;

    // 创建窗口并进入事件循环，阻塞到退出
    void Run();

private:
    // ---- 鼠标按键跨帧状态 ----
    struct MouseButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        double press_time = 0.0;
        bool moved_since_press = false;
    };

    // ---- 键盘按键跨帧状态 ----
    struct KeyButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        int  click_count = 0;
    };

    // ---- 帧内累计事件 ----
    struct FrameEvents {
        int left_clicks   = 0;
        int right_clicks  = 0;
        int middle_clicks = 0;
        jpov::ClickEvent left_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent right_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent middle_clicks_detail[jpov::kMaxClicksPerFrame];
    };

    double mouse_x_       = 0.0;
    double mouse_y_       = 0.0;
    double mouse_last_x_  = 0.0;
    double mouse_last_y_  = 0.0;
    double scroll_delta_  = 0.0;

    MouseButtonState left_btn_;
    MouseButtonState right_btn_;
    MouseButtonState middle_btn_;
    KeyButtonState keys_[jpov::kMaxKeyCode];
    FrameEvents frame_;
    double frame_start_time_ = 0.0;
    Config config_;
    GLFWwindow* window_ = nullptr;

    // Renderer（GL 复杂度消化器）
    std::unique_ptr<jpov::Renderer> renderer_;

    // 透视相机（内部持有，用户可通过 OneIteration 更新）
    jpov::Camera camera_;

    // ---- GLFW 回调（静态转发） ----
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods);
    static void OnMouseMove(GLFWwindow* window, double xpos, double ypos);
    static void OnScroll(GLFWwindow* window, double xoffset, double yoffset);
    static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods);

    void HandleMouseButton(int button, int action, double now);
    void HandleMouseMove(double xpos, double ypos);
    void HandleScroll(double yoffset);
    void HandleKey(int key, int scancode, int action, int mods);

    void CaptureInput(jpov::InputSnapshot* input /*output*/);
    void RenderCommands(const jpov::RenderCommandList& cmds /*input*/);

    double FrameInterval() const;

    static void FlushMouseButton(const MouseButtonState& btn,
                                 int click_count,
                                 const jpov::ClickEvent* click_detail,
                                 jpov::MouseState* out /*output*/,
                                 jpov::ClickEvent* out_clicks /*output*/);
    void FlushKeyboard(jpov::InputSnapshot* input /*output*/);

    static constexpr double kClickDelta = 0.3;
};

#endif  // JPOV_JPOV_H_

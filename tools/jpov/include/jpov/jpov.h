#ifndef JPOV_JPOV_H_
#define JPOV_JPOV_H_

#include <cstdint>
#include <memory>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#endif

#include <GLFW/glfw3.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/renderer.h"

// ============================================================================
// JPOV — 轻型渲染窗口框架
//
// 生命周期：
//   1. Init(config)  — 创建 GL context + Renderer
//   2. Run()         — 事件循环（多帧）
//    或
//      RunOnce()    — 单帧 → 截图输出
//   3. Finalize()    — 销毁所有资源
//
//  Run() 内部每帧调用 RunOnceInternal()，与 RunOnce() 共享相同的渲染核心，
//  确保在线与离线输出一致。
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
//       MyApp app(cfg);
//       app.Init();
//       app.Run();
//       app.Finalize();
//   }
// ============================================================================
class JPOV {
public:
    struct FontEntry {
        const char* path;         // TTF/TTC 文件路径，UTF-8
        int         ttc_index   = 0;  // TTC font index（单 TTF 忽略）
        const char* alias;            // 字体别名，DrawText 通过它引用
        // 最多允许注册 kMaxFonts 种字体
    };

    static constexpr int kMaxFonts = 10;

    struct Config {
        const char* title        = "JPOV";
        int         width        = 1280;
        int         height       = 720;
        bool        resizable    = true;
        bool        fullscreen   = false;
        bool        show_console = false;  // Windows 下是否显示命令行窗口

        // font_entries: 用户注册的字体列表（最多 kMaxFonts 项）
        // 每个 FontEntry 包含字体文件路径、TTC index、用户指定的别名。
        // 加载失败或别名重复时 Init() 会 LOG(FATAL) crash。
        // 空列表时 JPOV 尝试加载内置默认字体。
        std::vector<FontEntry> fonts;

        // headless = true: 无可见窗口（适用于 CI / 后台截图）
        // headless = false: 正常窗口（Run() 进入事件循环）
        bool        headless     = false;

        // OneIteration 调用帧率（Hz），默认 60。
        // Run() 按此帧率调度 OneIteration，实际帧率受 vsync 限制。
        // 设为 0 表示不限制（尽可能快）。
        int target_fps = 60;
    };

    explicit JPOV(Config cfg);
    virtual ~JPOV();

    // ---- 生命周期 ----

    // Init: 创建 GL context + Renderer。
    // headless=false 时创建可见窗口并注册 GLFW 回调。
    // headless=true  时创建隐藏窗口（仅用于 GL context）。
    // Pre-condition: 未调用过 Init() 或已调用 Finalize()
    void Init();

    // Finalize: 销毁 Renderer + GL context + 窗口。
    // 调用后可以再次 Init()。
    void Finalize();

    // ---- 运行模式 ----

    // Run: 窗口事件循环（阻塞）。
    // 每帧：PollEvents → CaptureInput → RunOnceInternal → Present → SwapBuffers。
    // Pre-condition: Init() 已调用且 window_ 非空
    void Run();

    // RunOnce: 单帧执行 + 截图输出
    //
    // 模拟一帧的完整流程：OneIteration → RunOnceInternal → SaveScreenshot
    // 输出图片为窗口尺寸（win_w × win_h），模拟 Present 到窗口后的视觉效果。
    //
    // input       — input: 模拟的输入快照
    // winfo       — input: 模拟的窗口信息（宽度/高度决定截图尺寸）
    // out_png_path — output: 截图保存路径
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: winfo.width > 0 && winfo.height > 0
    // Pre-condition: out_png_path 非空
    void RunOnce(const jpov::InputSnapshot& input,
                 const jpov::WindowInfo& winfo,
                 const char* out_png_path);

    // ---- 用户需实现的纯虚函数 ----

    // 用户实现的每帧渲染逻辑
    //
    // frame_count — input: 从 0 开始的帧计数器，单调递增
    // input       — input: 本帧输入快照（鼠标/键盘状态）
    // winfo       — input: 本帧窗口尺寸信息
    // cmds        — output: 填充渲染指令列表，帧末由框架消费。非空指针。
    //
    // Pre-condition: frame_count >= 0
    // Pre-condition: cmds != nullptr
    virtual void OneIteration(int64_t frame_count,
                              const jpov::InputSnapshot& input,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds /*output*/) = 0;

    // ---- 纹理管理 ----

    // RegisterTexture: 从 PNG 文件加载纹理到 GPU。
    //
    // 返回纹理 ID 用于 DrawImage。
    // 同一文件路径仅加载一次，重复调用返回相同 ID。
    // 加载失败 → LOG(FATAL) crash。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: filepath 非空
    uint32_t RegisterTexture(const std::string& filepath);

    // RegisterTexture: 注册已有 GL 纹理对象。
    //
    // 调用者自行管理 GL 纹理生命周期。
    // JPOV 仅在 ReleaseTexture 时移除记录，不 delete 外部 GL 纹理。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: gl_texture_id != 0, width > 0, height > 0
    uint32_t RegisterTexture(uint32_t gl_texture_id, int width, int height);

    // ReleaseTexture: 释放纹理。
    //
    // 纹理 ID 不存在 → 静默忽略。
    // Pre-condition: Init() 已调用
    void ReleaseTexture(uint32_t texture_id);

private:
    // ---- 核心渲染步骤（Run 和 RunOnce 共享） ----
    //
    // 1. 调用 OneIteration → cmds
    // 2. BeginFrame(render_resolution)
    // 3. Render(cmds)  → 绘制到 FBO
    //
    // Pre-condition: renderer_ 已初始化
    void RunOnceInternal(int64_t frame_count,
                         const jpov::InputSnapshot& input,
                         const jpov::WindowInfo& winfo);

    // ---- GLFW 输入状态 ----
    struct MouseButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        double press_time = 0.0;
        bool moved_since_press = false;
    };
    struct KeyButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        int  click_count = 0;
    };
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
    std::unique_ptr<jpov::Renderer> renderer_;
    pid_t xvfb_pid_    = 0;
    bool  started_xvfb_ = false;
    bool  initialized_  = false;

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

    static void FlushMouseButton(const MouseButtonState& btn /*input*/,
                                 int click_count /*input*/,
                                 const jpov::ClickEvent* click_detail /*input*/,
                                 jpov::MouseState* out /*output*/,
                                 jpov::ClickEvent* out_clicks /*output*/);
    void FlushKeyboard(jpov::InputSnapshot* input /*output*/);

    static constexpr double kClickDelta = 0.3;
};

#endif  // JPOV_JPOV_H_

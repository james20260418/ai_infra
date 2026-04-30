// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环：
//   1. 创建 GLFW 窗口
//   2. 每帧：CaptureInput() → OneIteration() → RenderCommands()
//   3. 窗口关闭后清理退出

#include "tools/jpov/include/jpov/jpov.h"

#include <GLFW/glfw3.h>
#include <glog/logging.h>

// Click 超时阈值：按下到释放间隔在此范围内才算一次 Click（单位：秒）
// 源自 UI 设计规范：200~400ms 内为典型点击，取 0.3s 作为硬边界。
// Doubles and duplicates:
//   双击自然跨两帧（按下者能前 帧 未释放而后帧首次释放）。
//   如果双按间隔很短（< CLICK_DELTA 但跨帧）：两个帧各自看到一次 Click。
//   短间隔双击被视为两次独立 Click，不做去重。
//   长按后释放（> CLICK_DELTA）：不算 Click。
constexpr double kClickDelta = 0.3;  // 秒

// ==================== 内部状态 ====================
//
// 窗口层在 Run() 循环内维护跨帧状态，辅助 EventCallback 填写内部队列。
// InputSnapshot 每帧结算一次，提交给 OneIteration。

namespace {

// 鼠标按键的跨帧跟踪状态
struct MouseButtonState {
    // 鼠标当前是否被按下（用于跨帧 Drag 判定）
    // true  ≈ 从本帧某个时刻（或之前）开始 mouse_down = true
    // false ≈ 本帧开始前 mouse_down = false
    bool is_down = false;

    // 最近一次按下的 GLFW 时间戳（glfwGetTime() 数值，秒）
    // 只有当 is_down 从 false→true 时更新。
    // 释放事件使用此时间戳与 Click 超时做比较：
    //   若 glfwGetTime() - press_time < kClickDelta → 判定为 Click
    //   否则 → 忽略
    //
    // 帧内时序 case（mouse down 为 true 的帧）：
    //   A) 同一帧内：释放 → 按下 → 释放
    //      第一次释放：press_time 来自上一帧按下，检查间隔→可能是 Click
    //      第二次释放：间隔很短（本帧按下），也符合→也是 Click
    //      结论：帧内三次点击序列产生两个 Click
    //   B) 按下状态跨帧，在本帧释放：
    //      press_time 是几帧前设置的，间隔 > kClickDelta → 不算 Click
    double press_time = 0.0;
};

// 三键（左/右/中）的跨帧状态
MouseButtonState g_mouse_left;
MouseButtonState g_mouse_right;
MouseButtonState g_mouse_middle;

// 本帧累计的鼠标事件（CaptureInput 每帧开始时重置）
int g_left_clicks = 0;
int g_right_clicks = 0;
int g_middle_clicks = 0;

// 本帧鼠标位置跟踪
double g_mouse_x = 0.0;
double g_mouse_y = 0.0;
double g_mouse_last_x = 0.0;
double g_mouse_last_y = 0.0;
double g_scroll_delta = 0.0;

// GLFW 窗口指针（CaptureInput / callbacks 使用）
GLFWwindow* g_window = nullptr;

// ---- GLFW 回调函数 ----

// 鼠标按钮回调
// Pre-condition: window 非空（GLFW 保证）
void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    (void)window;
    (void)mods;

    double now = glfwGetTime();

    MouseButtonState* btn = nullptr;
    int* click_count = nullptr;

    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
            btn = &g_mouse_left;
            click_count = &g_left_clicks;
            break;
        case GLFW_MOUSE_BUTTON_RIGHT:
            btn = &g_mouse_right;
            click_count = &g_right_clicks;
            break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            btn = &g_mouse_middle;
            click_count = &g_middle_clicks;
            break;
        default:
            return;
    }

    if (action == GLFW_PRESS) {
        // 记录按下时刻
        btn->press_time = now;
        btn->is_down = true;
    } else if (action == GLFW_RELEASE) {
        // 检查是否满足 Click 条件
        double elapsed = now - btn->press_time;
        if (elapsed < kClickDelta && *click_count < jpov::kMaxClicksPerFrame) {
            ++(*click_count);
        }
        btn->is_down = false;
    }
}

// 鼠标移动回调
void OnMouseMove(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    g_mouse_x = xpos;
    g_mouse_y = ypos;
}

// 滚轮回调
void OnScroll(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    g_scroll_delta += yoffset;
}

}  // anonymous namespace

JPOV::JPOV(Config cfg)
    : config_(cfg) {
}

JPOV::~JPOV() = default;

void JPOV::Run() {
    LOG(INFO) << "JPOV::Run() — starting window loop";
    LOG(INFO) << "  title:       " << config_.title;
    LOG(INFO) << "  size:        " << config_.width << "x" << config_.height;
    LOG(INFO) << "  resizable:   " << (config_.resizable ? "yes" : "no");
    LOG(INFO) << "  fullscreen:  " << (config_.fullscreen ? "yes" : "no");
    LOG(INFO) << "  show_console: " << (config_.show_console ? "yes" : "no");

    if (!glfwInit()) {
        LOG(FATAL) << "glfwInit() failed";
    }

    // 创建窗口
    g_window = glfwCreateWindow(config_.width, config_.height,
                                config_.title, nullptr, nullptr);
    if (!g_window) {
        glfwTerminate();
        LOG(FATAL) << "glfwCreateWindow() failed";
    }

    glfwMakeContextCurrent(g_window);

    // 注册回调
    glfwSetMouseButtonCallback(g_window, OnMouseButton);
    glfwSetCursorPosCallback(g_window, OnMouseMove);
    glfwSetScrollCallback(g_window, OnScroll);

    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(g_window)) {
            break;
        }

        CaptureInput();

        // 构造输入结构
        jpov::InputSnapshot input{};

        // ---- 鼠标位置 ----
        input.mouse_x = static_cast<float>(g_mouse_x);
        input.mouse_y = static_cast<float>(g_mouse_y);
        input.mouse_dx = static_cast<float>(g_mouse_x - g_mouse_last_x);
        input.mouse_dy = static_cast<float>(g_mouse_y - g_mouse_last_y);
        g_mouse_last_x = g_mouse_x;
        g_mouse_last_y = g_mouse_y;

        // 滚轮
        input.scroll_delta = static_cast<float>(g_scroll_delta);

        // ---- 鼠标左键 ----
        if (g_left_clicks > 0) {
            input.left.raw = static_cast<int8_t>(g_left_clicks);
            // TODO: fill left_clicks[] with actual click positions
        } else if (g_mouse_left.is_down) {
            input.left.raw = -1;  // Drag
        } else {
            input.left.raw = 0;   // None
        }

        // ---- 鼠标右键 ----
        if (g_right_clicks > 0) {
            input.right.raw = static_cast<int8_t>(g_right_clicks);
        } else if (g_mouse_right.is_down) {
            input.right.raw = -1;
        } else {
            input.right.raw = 0;
        }

        // ---- 鼠标中键 ----
        if (g_middle_clicks > 0) {
            input.middle.raw = static_cast<int8_t>(g_middle_clicks);
        } else if (g_mouse_middle.is_down) {
            input.middle.raw = -1;
        } else {
            input.middle.raw = 0;
        }

        // 构造窗口信息
        int fb_w, fb_h;
        glfwGetFramebufferSize(g_window, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 渲染指令列表
        jpov::RenderCommandList cmds;

        OneIteration(frame, input, winfo, cmds);

        RenderCommands(cmds);

        glfwSwapBuffers(g_window);
        glfwPollEvents();

        // 帧末清理：重置帧内累计事件
        g_left_clicks = 0;
        g_right_clicks = 0;
        g_middle_clicks = 0;
        g_scroll_delta = 0.0;

        ++frame;
    }

    glfwDestroyWindow(g_window);
    glfwTerminate();
    g_window = nullptr;

    LOG(INFO) << "JPOV::Run() — exiting";
}

void JPOV::CaptureInput() {
    // CaptureInput 由 GLFW 回调驱动（glfwPollEvents 在 Run() 循环中调用）
    // 本函数不做额外工作——事件记录已在回调中完成。
    // 帧末结算动作在 Run() 循环中完成。
}

void JPOV::RenderCommands(const jpov::RenderCommandList& cmds) {
    // 暂未实现：遍历 cmds，调用 OpenGL 绘制
    (void)cmds;
}

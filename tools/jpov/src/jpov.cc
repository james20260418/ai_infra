// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环：
//   1. 创建 GLFW 窗口
//   2. 每帧：CaptureInput() → OneIteration() → RenderCommands()
//   3. 窗口关闭后清理退出

#include "tools/jpov/include/jpov/jpov.h"

#include <glog/logging.h>

// ========== GLFW 静态回调 ==========
//
// GLFW 回调都是 C-style free function，通过 glfwSetWindowUserPointer 间接
// 转发到 JPOV 实例的 Handle* 方法。

void JPOV::OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleMouseButton(button, action, glfwGetTime());
    }
}

void JPOV::OnMouseMove(GLFWwindow* window, double xpos, double ypos) {
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleMouseMove(xpos, ypos);
    }
}

void JPOV::OnScroll(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleScroll(0.0, yoffset);
    }
}

void JPOV::OnKey(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)mods;
    (void)scancode;
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleKey(key, scancode, action, mods);
    }
}

// ========== 实例方法 ==========

JPOV::JPOV(Config cfg)
    : config_(cfg), window_(nullptr) {
}

JPOV::~JPOV() = default;

void JPOV::Run() {
    LOG(INFO) << "JPOV::Run() — starting window loop";
    LOG(INFO) << "  title:        " << config_.title;
    LOG(INFO) << "  size:         " << config_.width << "x" << config_.height;
    LOG(INFO) << "  resizable:    " << (config_.resizable ? "yes" : "no");
    LOG(INFO) << "  fullscreen:   " << (config_.fullscreen ? "yes" : "no");
    LOG(INFO) << "  show_console: " << (config_.show_console ? "yes" : "no");
    LOG(INFO) << "  target_fps:   " << config_.target_fps;

    if (!glfwInit()) {
        LOG(FATAL) << "glfwInit() failed";
    }

    // 创建窗口
    window_ = glfwCreateWindow(config_.width, config_.height,
                               config_.title, nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        LOG(FATAL) << "glfwCreateWindow() failed";
    }

    glfwMakeContextCurrent(window_);
    glfwSetWindowUserPointer(window_, this);

    // 注册回调
    glfwSetMouseButtonCallback(window_, OnMouseButton);
    glfwSetCursorPosCallback(window_, OnMouseMove);
    glfwSetScrollCallback(window_, OnScroll);
    glfwSetKeyCallback(window_, OnKey);

    // 目标帧间隔（秒）
    double frame_interval = (config_.target_fps > 0) ? (1.0 / config_.target_fps) : 0.0;

    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(window_)) {
            break;
        }

        double frame_start = glfwGetTime();

        // 1. 采集输入
        jpov::InputSnapshot input{};
        CaptureInput(&input);

        // 2. 窗口信息
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 3. 用户渲染逻辑
        jpov::RenderCommandList cmds;
        OneIteration(frame, input, winfo, &cmds);

        // 4. 消费渲染指令
        RenderCommands(cmds);

        // 5. 交换缓冲并轮询事件
        glfwSwapBuffers(window_);
        glfwPollEvents();

        // 6. 帧率控制
        if (frame_interval > 0.0) {
            double elapsed = glfwGetTime() - frame_start;
            double remaining = frame_interval - elapsed;
            if (remaining > 0.0) {
                // 用 glfwWaitEventsTimeout 让出 CPU 同时仍可响应事件
                // 但这里用 sleep 更干净——事件已在 glfwPollEvents 中处理完毕
                // 简单 spin-wait 不可取，用平台 sleep
                // glfwWaitEventsTimeout 会重新触发事件循环，可能不恰当
                // 用系统 sleep
                double sleep_s = remaining;
                // 粗略睡眠（毫秒级精度）
                struct timespec ts;
                ts.tv_sec  = static_cast<time_t>(sleep_s);
                ts.tv_nsec = static_cast<long>((sleep_s - ts.tv_sec) * 1e9);
                nanosleep(&ts, nullptr);
            }
        }

        ++frame;
    }

    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;

    LOG(INFO) << "JPOV::Run() — exiting (" << frame << " frames)";
}

void JPOV::CaptureInput(jpov::InputSnapshot* input) {
    CHECK_NOTNULL(input);

    // ---- 鼠标位置 ----
    input->mouse_x = static_cast<float>(mouse_x_);
    input->mouse_y = static_cast<float>(mouse_y_);
    input->mouse_dx = static_cast<float>(mouse_x_ - mouse_last_x_);
    input->mouse_dy = static_cast<float>(mouse_y_ - mouse_last_y_);
    mouse_last_x_ = mouse_x_;
    mouse_last_y_ = mouse_y_;

    // ---- 滚轮 ----
    input->scroll_delta = static_cast<float>(scroll_delta_);

    // ---- 鼠标左键 ----
    if (frame_.left_clicks > 0) {
        input->left.raw = static_cast<int8_t>(frame_.left_clicks);
        // TODO: fill left_clicks[] with actual click positions
    } else if (left_btn_.pressed_this_frame && left_btn_.is_down) {
        // 整帧内都是 down：本帧收到了 GLFW_PRESS，且帧末仍处于按下状态
        input->left.raw = -1;  // Drag
    } else {
        input->left.raw = 0;   // None
    }

    // ---- 鼠标右键 ----
    if (frame_.right_clicks > 0) {
        input->right.raw = static_cast<int8_t>(frame_.right_clicks);
    } else if (right_btn_.pressed_this_frame && right_btn_.is_down) {
        input->right.raw = -1;
    } else {
        input->right.raw = 0;
    }

    // ---- 鼠标中键 ----
    if (frame_.middle_clicks > 0) {
        input->middle.raw = static_cast<int8_t>(frame_.middle_clicks);
    } else if (middle_btn_.pressed_this_frame && middle_btn_.is_down) {
        input->middle.raw = -1;
    } else {
        input->middle.raw = 0;
    }

    // ---- 帧末重置帧内累计 ----
    frame_.left_clicks   = 0;
    frame_.right_clicks  = 0;
    frame_.middle_clicks = 0;
    scroll_delta_        = 0.0;

    // 重置 pressed_this_frame（下一帧重新统计）
    left_btn_.pressed_this_frame   = false;
    right_btn_.pressed_this_frame  = false;
    middle_btn_.pressed_this_frame = false;
}

void JPOV::RenderCommands(const jpov::RenderCommandList& cmds) {
    // 暂未实现：遍历 cmds，调用 OpenGL 绘制
    (void)cmds;
}

// ========== 事件处理（私有方法） ==========

void JPOV::HandleMouseButton(int button, int action, double now) {
    MouseButtonState* btn = nullptr;
    int* click_count = nullptr;

    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
            btn = &left_btn_;
            click_count = &frame_.left_clicks;
            break;
        case GLFW_MOUSE_BUTTON_RIGHT:
            btn = &right_btn_;
            click_count = &frame_.right_clicks;
            break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            btn = &middle_btn_;
            click_count = &frame_.middle_clicks;
            break;
        default:
            return;
    }

    if (action == GLFW_PRESS) {
        btn->press_time = now;
        btn->pressed_this_frame = true;
        btn->is_down = true;
    } else if (action == GLFW_RELEASE) {
        double elapsed = now - btn->press_time;
        if (elapsed < kClickDelta && *click_count < jpov::kMaxClicksPerFrame) {
            ++(*click_count);
        }
        btn->is_down = false;
    }
}

void JPOV::HandleMouseMove(double xpos, double ypos) {
    mouse_x_ = xpos;
    mouse_y_ = ypos;
}

void JPOV::HandleScroll(double /*xoffset*/, double yoffset) {
    scroll_delta_ += yoffset;
}

void JPOV::HandleKey(int /*key*/, int /*scancode*/, int /*action*/, int /*mods*/) {
    // 暂未实现键盘
}

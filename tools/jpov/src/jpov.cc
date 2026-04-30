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

void JPOV::OnScroll(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleScroll(yoffset);
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

    // 目标帧间隔（秒），用于帧率控制
    double frame_interval = FrameInterval();

    // 初始帧开始时间
    frame_start_time_ = glfwGetTime();

    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(window_)) {
            break;
        }

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

        // 6. 帧率控制：如果本帧提前完成，sleep 到目标帧间隔
        double elapsed = glfwGetTime() - frame_start_time_;
        double remaining = frame_interval - elapsed;
        if (remaining > 0.0) {
            // remaining < 1，所以 tv_sec 恒为 0，直接 sleep 微秒
            long us = static_cast<long>(remaining * 1e6);
            struct timespec ts = {0, us * 1000};
            nanosleep(&ts, nullptr);
        }

        ++frame;
    }

    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;

    LOG(INFO) << "JPOV::Run() — exiting (" << frame << " frames)";
}

void JPOV::FlushMouseButton(jpov::MouseState* out,
                              jpov::ClickEvent* out_clicks,
                              const MouseButtonState& btn,
                              int click_count,
                              const jpov::ClickEvent* click_detail,
                              double now) {
    int8_t raw;
    if (click_count > 0) {
        raw = static_cast<int8_t>(click_count);
        for (int i = 0; i < click_count && i < jpov::kMaxClicksPerFrame; ++i) {
            out_clicks[i] = click_detail[i];
        }
    } else if (btn.is_down && (now - btn.press_time) >= kMinDragDuration) {
        raw = -1;  // Drag
    } else {
        raw = 0;   // None
    }
    out->raw = raw;
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

    // ---- 更新本帧开始时间 ----
    // 用于 ClickEvent time_ratio 计算。在 CaptureInput 开头设而非帧末设，
    // 这样 HandleMouseButton 回调（由 glfwPollEvents 触发）拿到的
    // frame_start_time_ 是本帧的开始时刻。
    frame_start_time_ = glfwGetTime();

    // ---- 鼠标三键状态结算 ----
    double now = glfwGetTime();

    FlushMouseButton(&input->left,   input->left_clicks,
                     left_btn_,   frame_.left_clicks,   frame_.left_clicks_detail, now);
    FlushMouseButton(&input->right,  input->right_clicks,
                     right_btn_,  frame_.right_clicks,  frame_.right_clicks_detail, now);
    FlushMouseButton(&input->middle, input->middle_clicks,
                     middle_btn_, frame_.middle_clicks, frame_.middle_clicks_detail, now);

    // ---- 帧末重置帧内累计 ----
    frame_.left_clicks   = 0;
    frame_.right_clicks  = 0;
    frame_.middle_clicks = 0;
    scroll_delta_        = 0.0;

    left_btn_.released_this_frame   = false;
    right_btn_.released_this_frame  = false;
    middle_btn_.released_this_frame = false;
}

void JPOV::RenderCommands(const jpov::RenderCommandList& cmds) {
    // 暂未实现：遍历 cmds，调用 OpenGL 绘制
    (void)cmds;
}

// ========== 事件处理（私有方法） ==========

void JPOV::HandleMouseButton(int button, int action, double now) {
    // 将 GLFW button 映射为 (状态, click计数, click详情池)
    struct ButtonSlot {
        MouseButtonState* state;
        int*             click_count;
        jpov::ClickEvent* click_pool;
    };

    ButtonSlot slot;
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
            slot = {&left_btn_, &frame_.left_clicks, frame_.left_clicks_detail};
            break;
        case GLFW_MOUSE_BUTTON_RIGHT:
            slot = {&right_btn_, &frame_.right_clicks, frame_.right_clicks_detail};
            break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            slot = {&middle_btn_, &frame_.middle_clicks, frame_.middle_clicks_detail};
            break;
        default:
            return;
    }

    if (action == GLFW_PRESS) {
        slot.state->press_time = now;
        slot.state->is_down = true;
    } else if (action == GLFW_RELEASE) {
        slot.state->released_this_frame = true;

        double elapsed = now - slot.state->press_time;
        if (elapsed < kClickDelta && *slot.click_count < jpov::kMaxClicksPerFrame) {
            int idx = *slot.click_count;
            slot.click_pool[idx].x = static_cast<float>(mouse_x_);
            slot.click_pool[idx].y = static_cast<float>(mouse_y_);

            double target_interval = FrameInterval();
            double frame_elapsed = now - frame_start_time_;
            slot.click_pool[idx].time_ratio = static_cast<float>(std::min(frame_elapsed / target_interval, 1.0));

            ++(*slot.click_count);
        }
        slot.state->is_down = false;
    }
}

void JPOV::HandleMouseMove(double xpos, double ypos) {
    mouse_x_ = xpos;
    mouse_y_ = ypos;
}

void JPOV::HandleScroll(double yoffset) {
    scroll_delta_ += yoffset;
}

double JPOV::FrameInterval() const {
    return (config_.target_fps > 0) ? (1.0 / config_.target_fps) : (1.0 / 60.0);
}

void JPOV::HandleKey(int /*key*/, int /*scancode*/, int /*action*/, int /*mods*/) {
    // 暂未实现键盘
}

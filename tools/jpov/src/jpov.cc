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
    // 帧间隔（秒），用于帧率控制 + time_ratio 估算
    double frame_interval = (config_.target_fps > 0) ? (1.0 / config_.target_fps) : (1.0 / 60.0);

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

        // 6. 帧率控制
        double elapsed = glfwGetTime() - frame_start_time_;
        double remaining = frame_interval - elapsed;
        if (remaining > 0.0) {
            struct timespec ts;
            ts.tv_sec  = static_cast<time_t>(remaining);
            ts.tv_nsec = static_cast<long>((remaining - ts.tv_sec) * 1e9);
            nanosleep(&ts, nullptr);
        }

        // 更新下一帧开始时间（在帧末设，CaptureInput 开头会清 frame_start_time_）
        // 但这里不清——CaptureInput 开头设 frame_start_time_ = glfwGetTime()

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

    // ---- 更新本帧开始时间（用于 ClickEvent time_ratio 计算） ----
    // 在捕获输入前设置，这样 HandleMouseButton 回调在 glfwPollEvents 中
    // 触发时 frame_start_time_ 已经是正确的本帧开始时刻。
    frame_start_time_ = glfwGetTime();

    // ---- 鼠标左键 ----
    if (frame_.left_clicks > 0) {
        input->left.raw = static_cast<int8_t>(frame_.left_clicks);
        for (int i = 0; i < frame_.left_clicks && i < jpov::kMaxClicksPerFrame; ++i) {
            input->left_clicks[i] = frame_.left_clicks_detail[i];
        }
    } else if (left_btn_.is_down && !left_btn_.released_this_frame) {
        // 本帧从未释放过且帧末仍按下 = 整帧 drag
        input->left.raw = -1;  // Drag
    } else {
        input->left.raw = 0;   // None
    }

    // ---- 鼠标右键 ----
    if (frame_.right_clicks > 0) {
        input->right.raw = static_cast<int8_t>(frame_.right_clicks);
        for (int i = 0; i < frame_.right_clicks && i < jpov::kMaxClicksPerFrame; ++i) {
            input->right_clicks[i] = frame_.right_clicks_detail[i];
        }
    } else if (right_btn_.is_down && !right_btn_.released_this_frame) {
        input->right.raw = -1;
    } else {
        input->right.raw = 0;
    }

    // ---- 鼠标中键 ----
    if (frame_.middle_clicks > 0) {
        input->middle.raw = static_cast<int8_t>(frame_.middle_clicks);
        for (int i = 0; i < frame_.middle_clicks && i < jpov::kMaxClicksPerFrame; ++i) {
            input->middle_clicks[i] = frame_.middle_clicks_detail[i];
        }
    } else if (middle_btn_.is_down && !middle_btn_.released_this_frame) {
        input->middle.raw = -1;
    } else {
        input->middle.raw = 0;
    }

    // ---- 帧末重置帧内累计 ----
    frame_.left_clicks   = 0;
    frame_.right_clicks  = 0;
    frame_.middle_clicks = 0;
    scroll_delta_        = 0.0;

    // 重置 released_this_frame（下一帧重新统计）
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
        btn->is_down = true;
    } else if (action == GLFW_RELEASE) {
        btn->released_this_frame = true;

        double elapsed = now - btn->press_time;
        if (elapsed < kClickDelta && *click_count < jpov::kMaxClicksPerFrame) {
            // 确定用哪个 ClickEvent 池
            jpov::ClickEvent* pool = nullptr;
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                pool = frame_.left_clicks_detail;
            } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                pool = frame_.right_clicks_detail;
            } else {
                pool = frame_.middle_clicks_detail;
            }
            // 填入释放位置和 time_ratio
            int idx = *click_count;
            pool[idx].x = static_cast<float>(mouse_x_);
            pool[idx].y = static_cast<float>(mouse_y_);
            // time_ratio = (释放时刻 - 帧开始时间) / 目标帧间隔
            // 近似值：帧实际时长未知，用目标帧间隔估算
            double target_interval = (config_.target_fps > 0) ? (1.0 / config_.target_fps) : (1.0 / 60.0);
            double frame_elapsed = now - frame_start_time_;
            pool[idx].time_ratio = static_cast<float>(std::min(frame_elapsed / target_interval, 1.0));

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

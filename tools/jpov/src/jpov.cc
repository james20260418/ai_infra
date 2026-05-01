// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环：
//   1. 创建 GLFW 窗口
//   2. 初始化 Renderer（FBO + shader + VBO）
//   3. 每帧：BeginFrame() → CaptureInput() → OneIteration() → Render() → Present()
//   4. 窗口关闭后清理退出

#include "tools/jpov/include/jpov/jpov.h"

#include "tools/jpov/src/renderer.h"

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

    // ---- 初始化 Renderer（FBO + shader + VBO） ----
    renderer_ = std::make_unique<jpov::Renderer>();
    renderer_->Init();

    // ---- 设置默认相机（3D 预留） ----
    camera_.position = {0.0f, 0.0f, 10.0f};
    camera_.target   = {0.0f, 0.0f, 0.0f};
    camera_.up       = {0.0f, 1.0f, 0.0f};
    camera_.fov      = 60.0f;
    camera_.near     = 0.1f;
    camera_.far      = 1000.0f;

    // 目标帧间隔（秒），用于帧率控制
    double frame_interval = FrameInterval();

    // 初始帧开始时间
    frame_start_time_ = glfwGetTime();

    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(window_)) {
            break;
        }

        // 1. 窗口信息
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 2. 绑定 FBO，清屏
        renderer_->BeginFrame();

        // 3. 采集输入
        jpov::InputSnapshot input{};
        CaptureInput(&input);

        // 4. 用户渲染逻辑（产出渲染指令列表）
        jpov::RenderCommandList cmds;
        OneIteration(frame, input, winfo, &cmds);

        // 5. 消费渲染指令（绘制到 FBO）
        renderer_->Render(cmds, camera_, winfo);

        // 6. FBO 窗口区域 → 默认 framebuffer（无缩放）
        renderer_->Present(window_, fb_w, fb_h);

        glfwSwapBuffers(window_);
        glfwPollEvents();

        // 7. 帧率控制
        double elapsed = glfwGetTime() - frame_start_time_;
        double remaining = frame_interval - elapsed;
        if (remaining > 0.0) {
            long us = static_cast<long>(remaining * 1e6);
            struct timespec ts = {0, us * 1000};
            nanosleep(&ts, nullptr);
        }

        ++frame;
    }

    // 清理
    renderer_.reset();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;

    LOG(INFO) << "JPOV::Run() — exiting (" << frame << " frames)";
}

void JPOV::FlushMouseButton(const MouseButtonState& btn /*input*/,
                              int click_count /*input*/,
                              const jpov::ClickEvent* click_detail /*input*/,
                              jpov::MouseState* out /*output*/,
                              jpov::ClickEvent* out_clicks /*output*/) {
    int8_t raw;
    if (click_count > 0) {
        raw = static_cast<int8_t>(click_count);
        for (int i = 0; i < click_count && i < jpov::kMaxClicksPerFrame; ++i) {
            out_clicks[i] = click_detail[i];
        }
    } else if (btn.is_down) {
        raw = btn.moved_since_press ? -1 : -2;  // Drag / Hold
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
    frame_start_time_ = glfwGetTime();

    // ---- 鼠标三键状态结算 ----
    FlushMouseButton(left_btn_,   frame_.left_clicks,   frame_.left_clicks_detail,
                     &input->left,   input->left_clicks);
    FlushMouseButton(right_btn_,  frame_.right_clicks,  frame_.right_clicks_detail,
                     &input->right,  input->right_clicks);
    FlushMouseButton(middle_btn_, frame_.middle_clicks, frame_.middle_clicks_detail,
                     &input->middle, input->middle_clicks);

    // ---- 键盘 ----
    FlushKeyboard(input);

    // ---- 帧末重置帧内累计 ----
    frame_.left_clicks   = 0;
    frame_.right_clicks  = 0;
    frame_.middle_clicks = 0;
    scroll_delta_        = 0.0;

    left_btn_.released_this_frame   = false;
    right_btn_.released_this_frame  = false;
    middle_btn_.released_this_frame = false;

    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        keys_[i].click_count = 0;
        keys_[i].released_this_frame = false;
    }
}

void JPOV::RenderCommands(const jpov::RenderCommandList& cmds) {
    // 不再使用：由 Renderer::Render() 代替
    (void)cmds;
}

// ========== 事件处理（私有方法） ==========

void JPOV::HandleMouseButton(int button, int action, double now) {
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
        slot.state->moved_since_press = false;
    } else if (action == GLFW_RELEASE) {
        slot.state->released_this_frame = true;

        bool should_click = !slot.state->moved_since_press;
        if (should_click && *slot.click_count < jpov::kMaxClicksPerFrame) {
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
    if (xpos != mouse_x_ || ypos != mouse_y_) {
        mouse_x_ = xpos;
        mouse_y_ = ypos;
        if (left_btn_.is_down)   left_btn_.moved_since_press = true;
        if (right_btn_.is_down)  right_btn_.moved_since_press = true;
        if (middle_btn_.is_down) middle_btn_.moved_since_press = true;
    }
}

void JPOV::HandleScroll(double yoffset) {
    scroll_delta_ += yoffset;
}

double JPOV::FrameInterval() const {
    return (config_.target_fps > 0) ? (1.0 / config_.target_fps) : (1.0 / 60.0);
}

void JPOV::HandleKey(int key, int /*scancode*/, int action, int /*mods*/) {
    if (key < 0 || key >= jpov::kMaxKeyCode) {
        return;
    }
    KeyButtonState& k = keys_[key];

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (!k.is_down) {
            k.is_down = true;
        }
    } else if (action == GLFW_RELEASE) {
        k.released_this_frame = true;
        ++k.click_count;
        k.is_down = false;
    }
}

void JPOV::FlushKeyboard(jpov::InputSnapshot* input /*output*/) {
    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        const KeyButtonState& k = keys_[i];
        int8_t raw;
        if (k.click_count > 0) {
            raw = static_cast<int8_t>(k.click_count);
            if (raw > jpov::kMaxClicksPerFrame) {
                raw = jpov::kMaxClicksPerFrame;
            }
        } else if (k.is_down) {
            raw = -2;  // Hold
        } else {
            raw = 0;   // None
        }
        input->keys[i].raw = raw;
    }
}

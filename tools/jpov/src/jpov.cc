// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环：
//   1. 创建 GLFW 窗口
//   2. 初始化 Renderer（shader + VBO）
//   3. 每帧：BeginFrame() → CaptureInput() → OneIteration() → Render() → Present()
//   4. 窗口关闭后清理退出

#include "tools/jpov/include/jpov/jpov.h"

#include "tools/jpov/src/renderer.h"

#include <glog/logging.h>

// ========== GLFW 静态回调 ==========

void JPOV::OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) self->HandleMouseButton(button, action, glfwGetTime());
}

void JPOV::OnMouseMove(GLFWwindow* window, double xpos, double ypos) {
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) self->HandleMouseMove(xpos, ypos);
}

void JPOV::OnScroll(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) self->HandleScroll(yoffset);
}

void JPOV::OnKey(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)mods;
    (void)scancode;
    auto* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) self->HandleKey(key, scancode, action, mods);
}

// ========== 实例方法 ==========

JPOV::JPOV(Config cfg) : config_(cfg), window_(nullptr) {}

JPOV::~JPOV() = default;

void JPOV::Run() {
    LOG(INFO) << "JPOV::Run() — starting";

    if (!glfwInit()) LOG(FATAL) << "glfwInit() failed";

    window_ = glfwCreateWindow(config_.width, config_.height,
                               config_.title, nullptr, nullptr);
    if (!window_) { glfwTerminate(); LOG(FATAL) << "glfwCreateWindow() failed"; }

    glfwMakeContextCurrent(window_);
    glfwSetWindowUserPointer(window_, this);
    glfwSetMouseButtonCallback(window_, OnMouseButton);
    glfwSetCursorPosCallback(window_, OnMouseMove);
    glfwSetScrollCallback(window_, OnScroll);
    glfwSetKeyCallback(window_, OnKey);

    // 初始化 Renderer（shader + VBO，FBO 在 BeginFrame 时按需创建）
    renderer_ = std::make_unique<jpov::Renderer>();
    renderer_->Init();

    double frame_interval = FrameInterval();
    frame_start_time_ = glfwGetTime();
    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(window_)) break;

        // 1. 窗口信息
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 2. 用户渲染逻辑（填充 RenderCommandList，含 render_resolution）
        jpov::RenderCommandList cmds;
        OneIteration(frame, jpov::InputSnapshot{}, winfo, &cmds);

        // 3. 使用用户声明的分辨率（fallback 到窗口尺寸）
        int rw = cmds.render_width  > 0 ? cmds.render_width  : fb_w;
        int rh = cmds.render_height > 0 ? cmds.render_height : fb_h;

        // 4. 绑定 FBO（检测分辨率变化，不变就复用）
        renderer_->BeginFrame(rw, rh);

        // 5. 采集输入
        jpov::InputSnapshot input{};
        CaptureInput(&input);

        // 6. 重新执行 OneIteration（这次带真实 input）
        cmds.Clear();
        OneIteration(frame, input, winfo, &cmds);

        // 7. 消费渲染指令
        rw = cmds.render_width  > 0 ? cmds.render_width  : fb_w;
        rh = cmds.render_height > 0 ? cmds.render_height : fb_h;
        renderer_->Render(cmds, jpov::Camera{}, winfo);

        // 8. FBO → 窗口
        renderer_->Present(window_, fb_w, fb_h);

        glfwSwapBuffers(window_);
        glfwPollEvents();

        // 帧率控制
        double elapsed = glfwGetTime() - frame_start_time_;
        double remaining = frame_interval - elapsed;
        if (remaining > 0.0) {
            long us = static_cast<long>(remaining * 1e6);
            struct timespec ts = {0, us * 1000};
            nanosleep(&ts, nullptr);
        }
        ++frame;
    }

    renderer_.reset();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
    LOG(INFO) << "JPOV::Run() — exiting (" << frame << " frames)";
}

void JPOV::FlushMouseButton(const MouseButtonState& btn,
                              int click_count,
                              const jpov::ClickEvent* click_detail,
                              jpov::MouseState* out,
                              jpov::ClickEvent* out_clicks) {
    int8_t raw;
    if (click_count > 0) {
        raw = static_cast<int8_t>(click_count);
        for (int i = 0; i < click_count && i < jpov::kMaxClicksPerFrame; ++i)
            out_clicks[i] = click_detail[i];
    } else if (btn.is_down) {
        raw = btn.moved_since_press ? -1 : -2;
    } else {
        raw = 0;
    }
    out->raw = raw;
}

void JPOV::CaptureInput(jpov::InputSnapshot* input) {
    CHECK_NOTNULL(input);
    input->mouse_x = static_cast<float>(mouse_x_);
    input->mouse_y = static_cast<float>(mouse_y_);
    input->mouse_dx = static_cast<float>(mouse_x_ - mouse_last_x_);
    input->mouse_dy = static_cast<float>(mouse_y_ - mouse_last_y_);
    mouse_last_x_ = mouse_x_;
    mouse_last_y_ = mouse_y_;
    input->scroll_delta = static_cast<float>(scroll_delta_);
    frame_start_time_ = glfwGetTime();

    FlushMouseButton(left_btn_,   frame_.left_clicks,   frame_.left_clicks_detail,
                     &input->left,   input->left_clicks);
    FlushMouseButton(right_btn_,  frame_.right_clicks,  frame_.right_clicks_detail,
                     &input->right,  input->right_clicks);
    FlushMouseButton(middle_btn_, frame_.middle_clicks, frame_.middle_clicks_detail,
                     &input->middle, input->middle_clicks);
    FlushKeyboard(input);

    frame_.left_clicks = frame_.right_clicks = frame_.middle_clicks = 0;
    scroll_delta_ = 0.0;
    left_btn_.released_this_frame = right_btn_.released_this_frame = middle_btn_.released_this_frame = false;
    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        keys_[i].click_count = 0;
        keys_[i].released_this_frame = false;
    }
}

void JPOV::RenderCommands(const jpov::RenderCommandList&) {}

void JPOV::HandleMouseButton(int button, int action, double now) {
    struct Slot { MouseButtonState* s; int* cc; jpov::ClickEvent* pool; };
    Slot slot;
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:   slot = {&left_btn_, &frame_.left_clicks, frame_.left_clicks_detail}; break;
        case GLFW_MOUSE_BUTTON_RIGHT:  slot = {&right_btn_, &frame_.right_clicks, frame_.right_clicks_detail}; break;
        case GLFW_MOUSE_BUTTON_MIDDLE: slot = {&middle_btn_, &frame_.middle_clicks, frame_.middle_clicks_detail}; break;
        default: return;
    }
    if (action == GLFW_PRESS) {
        slot.s->press_time = now; slot.s->is_down = true; slot.s->moved_since_press = false;
    } else if (action == GLFW_RELEASE) {
        slot.s->released_this_frame = true;
        bool should_click = !slot.s->moved_since_press;
        if (should_click && *slot.cc < jpov::kMaxClicksPerFrame) {
            int idx = *slot.cc;
            slot.pool[idx].x = static_cast<float>(mouse_x_);
            slot.pool[idx].y = static_cast<float>(mouse_y_);
            double ti = FrameInterval();
            double fe = now - frame_start_time_;
            slot.pool[idx].time_ratio = static_cast<float>(std::min(fe / ti, 1.0));
            ++(*slot.cc);
        }
        slot.s->is_down = false;
    }
}

void JPOV::HandleMouseMove(double xpos, double ypos) {
    if (xpos != mouse_x_ || ypos != mouse_y_) {
        mouse_x_ = xpos; mouse_y_ = ypos;
        if (left_btn_.is_down)   left_btn_.moved_since_press = true;
        if (right_btn_.is_down)  right_btn_.moved_since_press = true;
        if (middle_btn_.is_down) middle_btn_.moved_since_press = true;
    }
}

void JPOV::HandleScroll(double yoffset) { scroll_delta_ += yoffset; }

double JPOV::FrameInterval() const {
    return (config_.target_fps > 0) ? (1.0 / config_.target_fps) : (1.0 / 60.0);
}

void JPOV::HandleKey(int key, int, int action, int) {
    if (key < 0 || key >= jpov::kMaxKeyCode) return;
    auto& k = keys_[key];
    if (action == GLFW_PRESS || action == GLFW_REPEAT) { if (!k.is_down) k.is_down = true; }
    else if (action == GLFW_RELEASE) { k.released_this_frame = true; ++k.click_count; k.is_down = false; }
}

void JPOV::FlushKeyboard(jpov::InputSnapshot* input) {
    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        const auto& k = keys_[i];
        int8_t raw;
        if (k.click_count > 0) raw = static_cast<int8_t>(std::min(k.click_count, jpov::kMaxClicksPerFrame));
        else if (k.is_down) raw = -2;
        else raw = 0;
        input->keys[i].raw = raw;
    }
}

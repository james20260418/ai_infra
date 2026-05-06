// JPOV — 轻型渲染窗口框架 实现
//
// Run() 主循环（单次 OneIteration）：
//   1. 窗口信息 + 采集输入
//   2. OneIteration(input, winfo, &cmds) — 用户产出分辨率 + 绘制指令
//   3. BeginFrame(cmds.render_width/height) — 绑定/重建 FBO
//   4. Render(cmds) — 绘制到 FBO
//   5. Present() — FBO → 窗口
//
// 每帧只调用一次 OneIteration，用户在其中同时声明分辨率与绘制内容。

#include "tools/jpov/include/jpov/jpov.h"

#include <algorithm>
#include <glog/logging.h>

// ========== GLFW 静态回调 ==========

void JPOV::OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    CHECK(window != nullptr);
    JPOV* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleMouseButton(button, action, glfwGetTime());
    }
}

void JPOV::OnMouseMove(GLFWwindow* window, double xpos, double ypos) {
    CHECK(window != nullptr);
    JPOV* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleMouseMove(xpos, ypos);
    }
}

void JPOV::OnScroll(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    CHECK(window != nullptr);
    JPOV* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleScroll(yoffset);
    }
}

void JPOV::OnKey(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)mods;
    (void)scancode;
    CHECK(window != nullptr);
    JPOV* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleKey(key, scancode, action, mods);
    }
}

// ========== 实例方法 ==========

JPOV::JPOV(Config cfg) : config_(cfg), window_(nullptr) {}

JPOV::~JPOV() = default;

void JPOV::Run() {
    LOG(INFO) << "JPOV::Run() — starting";

    if (!glfwInit()) {
        LOG(FATAL) << "glfwInit() failed";
    }

    window_ = glfwCreateWindow(config_.width, config_.height,
                               config_.title, nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        LOG(FATAL) << "glfwCreateWindow() failed";
    }

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
        if (glfwWindowShouldClose(window_)) {
            break;
        }

        // 1. 采集输入（鼠标/键盘状态）
        jpov::InputSnapshot input{};
        CaptureInput(&input);

        // 2. 窗口信息
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 3. 用户渲染逻辑：一次调用产出分辨率 + 绘制指令
        jpov::RenderCommandList cmds;
        OneIteration(frame, input, winfo, &cmds);

        // 4. 绑定/创建 FBO（使用用户声明的分辨率）
        renderer_->BeginFrame(cmds.render_width, cmds.render_height);

        // 5. 消费渲染指令（绘制到 FBO）
        renderer_->Render(cmds, jpov::Camera{}, winfo);

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
        for (int i = 0; i < click_count && i < jpov::kMaxClicksPerFrame; ++i) {
            out_clicks[i] = click_detail[i];
        }
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

    frame_.left_clicks = 0;
    frame_.right_clicks = 0;
    frame_.middle_clicks = 0;
    scroll_delta_ = 0.0;
    left_btn_.released_this_frame = false;
    right_btn_.released_this_frame = false;
    middle_btn_.released_this_frame = false;

    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        keys_[i].click_count = 0;
        keys_[i].released_this_frame = false;
    }
}

void JPOV::RunOnce(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    const char* out_png_path) {
    CHECK_GT(winfo.width, 0);
    CHECK_GT(winfo.height, 0);
    CHECK(out_png_path != nullptr);

    // 确保 GL context 存在（隐藏窗口）
    bool need_gl = !window_;
    if (need_gl) {
        if (!glfwInit()) {
            LOG(FATAL) << "glfwInit() failed";
        }
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window_ = glfwCreateWindow(static_cast<int>(winfo.width),
                                   static_cast<int>(winfo.height),
                                   "JPOV_RunOnce", nullptr, nullptr);
        if (!window_) {
            glfwTerminate();
            LOG(FATAL) << "glfwCreateWindow() failed";
        }
        glfwMakeContextCurrent(window_);
    }

    // 确保 Renderer 已初始化
    if (!renderer_) {
        renderer_ = std::make_unique<jpov::Renderer>();
        renderer_->Init();
    }

    // 1. 用户渲染逻辑：产出分辨率 + 绘制指令
    jpov::RenderCommandList cmds;
    OneIteration(0, input, winfo, &cmds);

    // 2. 绑定/创建 FBO（使用用户声明的分辨率）
    renderer_->BeginFrame(cmds.render_width, cmds.render_height);

    // 3. 消费渲染指令（绘制到 FBO）
    renderer_->Render(cmds, jpov::Camera{}, winfo);

    // 4. 以窗口尺寸保存截图（模拟 Present 到窗口的效果）
    int win_w = static_cast<int>(winfo.width);
    int win_h = static_cast<int>(winfo.height);
    renderer_->SaveScreenshot(win_w, win_h, out_png_path);

    // 5. 清理临时 GL 资源
    if (need_gl) {
        renderer_.reset();
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

void JPOV::RenderCommands(const jpov::RenderCommandList&) {}

void JPOV::HandleMouseButton(int button, int action, double now) {
    struct Slot {
        MouseButtonState* s;
        int* cc;
        jpov::ClickEvent* pool;
    };
    Slot slot;
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
        slot.s->press_time = now;
        slot.s->is_down = true;
        slot.s->moved_since_press = false;
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
        mouse_x_ = xpos;
        mouse_y_ = ypos;
        if (left_btn_.is_down) {
            left_btn_.moved_since_press = true;
        }
        if (right_btn_.is_down) {
            right_btn_.moved_since_press = true;
        }
        if (middle_btn_.is_down) {
            middle_btn_.moved_since_press = true;
        }
    }
}

void JPOV::HandleScroll(double yoffset) {
    scroll_delta_ += yoffset;
}

double JPOV::FrameInterval() const {
    if (config_.target_fps > 0) {
        return 1.0 / config_.target_fps;
    }
    return 1.0 / 60.0;
}

void JPOV::HandleKey(int key, int, int action, int) {
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

void JPOV::FlushKeyboard(jpov::InputSnapshot* input) {
    for (int i = 1; i < jpov::kMaxKeyCode; ++i) {
        const KeyButtonState& k = keys_[i];
        int8_t raw;
        if (k.click_count > 0) {
            // Ensure click_count is within valid range for int8_t
            int count = std::min(k.click_count, jpov::kMaxClicksPerFrame);
            raw = static_cast<int8_t>(count);
        } else if (k.is_down) {
            raw = -2;
        } else {
            raw = 0;
        }
        input->keys[i].raw = raw;
    }
}

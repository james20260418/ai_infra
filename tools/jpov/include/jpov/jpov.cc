// JPOV — 轻型渲染窗口框架 实现
//
// 生命周期：
//   Init() → Run() 或 RunOnce() → Finalize()
//
// Run() 主循环（每帧）：
//   1. PollEvents + CaptureInput
//   2. RunOnceInternal(frame, input, winfo)
//   3. Present(window)
//   4. SwapBuffers
//
// RunOnce()：
//   1. RunOnceInternal(0, input, winfo)
//   2. SaveScreenshot(path)

#include "jpov.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <glog/logging.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// ========== 生命周期 ==========

JPOV::JPOV(Config cfg)
    : config_(cfg), window_(nullptr), initialized_(false) {}

JPOV::~JPOV() {
    if (initialized_) {
        Finalize();
    }
}

void JPOV::Init() {
    CHECK(!initialized_) << "JPOV already initialized. Call Finalize() first.";

    if (!glfwInit()) {
        if (!config_.headless) {
            LOG(FATAL) << "glfwInit() failed";
        }
#ifndef _WIN32
        const char* existing_display = getenv("DISPLAY");
        if (existing_display && existing_display[0] != '\0') {
            // DISPLAY 已设置（例如 CI 提前启动了 Xvfb），重试 glfwInit
            LOG(WARNING) << "glfwInit() failed but DISPLAY=" << existing_display
                         << " is set — retrying after short delay";
            sleep(1);
            if (!glfwInit()) {
                LOG(FATAL) << "glfwInit() still failed even with DISPLAY="
                           << existing_display;
            }
            started_xvfb_ = false;
        } else {
            LOG(WARNING) << "glfwInit() failed — attempting to start Xvfb on :99";
            int pid = fork();
            if (pid == 0) {
                // 子进程：启动 Xvfb
                execlp("Xvfb", "Xvfb", ":99", "-ac", "-screen", "0", "1280x720x24",
                       "-noreset", "+extension", "GLX", "+iglx", nullptr);
                _exit(1);
            }
            CHECK_GT(pid, 0) << "fork() failed";
            xvfb_pid_ = pid;
            // 设置 DISPLAY 环境变量，让 glfwInit 连到这个 display
            setenv("DISPLAY", ":99", 1);
            sleep(1);
            if (!glfwInit()) {
                LOG(FATAL) << "glfwInit() still failed after Xvfb launch";
            }
            started_xvfb_ = true;
        }
#else
        LOG(FATAL) << "glfwInit() failed — headless mode not supported on Windows without DISPLAY";
#endif
    }

    int win_w = config_.width;
    int win_h = config_.height;

    if (config_.headless) {
        // 隐藏窗口：仅用于 GL context
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    window_ = glfwCreateWindow(win_w, win_h, config_.title, nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        LOG(FATAL) << "glfwCreateWindow() failed";
    }

    glfwMakeContextCurrent(window_);

    if (!config_.headless) {
        glfwSetWindowUserPointer(window_, this);
        glfwSetMouseButtonCallback(window_, OnMouseButton);
        glfwSetCursorPosCallback(window_, OnMouseMove);
        glfwSetScrollCallback(window_, OnScroll);
        glfwSetKeyCallback(window_, OnKey);
    }

    // 将 Config::fonts（FontEntry 列表）转换为 (path, ttc_index, alias) 三元组
    std::vector<std::tuple<const char*, int, const char*>> font_tuples;
    font_tuples.reserve(config_.fonts.size());
    for (const auto& fe : config_.fonts) {
        font_tuples.emplace_back(fe.path, fe.ttc_index, fe.alias);
    }

    // 内置默认字体（path, ttc_index, alias）
    // 与用户 fonts 共享别名空间，同名 alias 会 crash
    // 注意：stb_truetype 对 CFF (PostScript outline) 格式支持不稳定，
    // 仅加载 TrueType outline (.ttf/.ttc) 字体。
    const std::vector<std::tuple<const char*, int, const char*>> kDefaultFonts = {
        {"tools/jpov/fonts/NotoSansCJK-Regular.ttc", 0, "CJK"},
        {"tools/jpov/fonts/DejaVuSans.ttf",            0, "Latin"},
    };

    renderer_ = std::make_unique<jpov::Renderer>();
    renderer_->Init(font_tuples, kDefaultFonts);

    initialized_ = true;
    LOG(INFO) << "JPOV::Init() — " << (config_.headless ? "headless" : "windowed")
              << " " << win_w << "x" << win_h;
}

void JPOV::Finalize() {
    CHECK(initialized_) << "JPOV not initialized.";

    renderer_.reset();

    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    glfwTerminate();

#ifndef _WIN32
    if (started_xvfb_ && xvfb_pid_ > 0) {
        kill(xvfb_pid_, SIGTERM);
        waitpid(xvfb_pid_, nullptr, 0);
        xvfb_pid_ = 0;
        started_xvfb_ = false;
        LOG(INFO) << "JPOV: Xvfb terminated";
    }
#endif

    initialized_ = false;
    LOG(INFO) << "JPOV::Finalize()";
}

// ========== 核心渲染步骤 ==========

void JPOV::RunOnceInternal(int64_t frame_count,
                            const jpov::InputSnapshot& input,
                            const jpov::WindowInfo& winfo) {
    CHECK(initialized_);

    jpov::RenderCommandList cmds;
    OneIteration(frame_count, input, winfo, &cmds);

    renderer_->BeginFrame(cmds.render_width, cmds.render_height);
    renderer_->Render(cmds, jpov::Camera{}, winfo);
}

// ========== 运行模式 ==========

void JPOV::Run() {
    CHECK(initialized_);
    CHECK(!config_.headless) << "Run() requires a visible window. "
                                "Use headless=false or call RunOnce() instead.";
    CHECK(window_ != nullptr);

    double frame_interval = FrameInterval();
    frame_start_time_ = glfwGetTime();
    int64_t frame = 0;

    while (true) {
        if (glfwWindowShouldClose(window_)) {
            break;
        }

        // 1. 采集输入 + 窗口信息
        jpov::InputSnapshot input{};
        CaptureInput(&input);

        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        jpov::WindowInfo winfo;
        winfo.width  = static_cast<float>(fb_w);
        winfo.height = static_cast<float>(fb_h);

        // 2. 核心渲染（OneIteration + BeginFrame + Render）
        RunOnceInternal(frame, input, winfo);

        // 3. Present 到窗口
        renderer_->Present(window_, fb_w, fb_h);

        glfwSwapBuffers(window_);
        glfwPollEvents();

        // 4. 帧率控制
        double elapsed = glfwGetTime() - frame_start_time_;
        double remaining = frame_interval - elapsed;
        if (remaining > 0.0) {
            long us = static_cast<long>(remaining * 1e6);
            struct timespec ts = {0, us * 1000};
            nanosleep(&ts, nullptr);
        }
        ++frame;
    }

    LOG(INFO) << "JPOV::Run() — exiting (" << frame << " frames)";
}

void JPOV::RunOnce(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    const char* out_png_path) {
    CHECK(initialized_);
    CHECK_GT(winfo.width, 0);
    CHECK_GT(winfo.height, 0);
    CHECK(out_png_path != nullptr);

    // 1. 核心渲染（与 Run() 完全一致）
    RunOnceInternal(0, input, winfo);

    // 2. 保存窗口尺寸截图
    int win_w = static_cast<int>(winfo.width);
    int win_h = static_cast<int>(winfo.height);
    renderer_->SaveScreenshot(win_w, win_h, out_png_path);
}

// ========== 输入采集 ==========

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

void JPOV::RenderCommands(const jpov::RenderCommandList&) {}

// ========== GLFW 事件处理 ==========

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

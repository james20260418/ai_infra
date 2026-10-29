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

#include "tools/jpov/src/debug_overlay/debug_overlay_renderer.h"
#include "tools/jpov/src/gpu_info_query.h"

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
    CHECK(window != nullptr);
    JPOV* self = static_cast<JPOV*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->HandleKey(key, scancode, action, mods);
    }
}

// ========== 生命周期 ==========

JPOV::JPOV(Config cfg)
    : config_(cfg),
      window_(nullptr),
      debug_overlay_visible_(cfg.debug_overlay),
      initialized_(false) {}

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

    // 内置默认字体壳（保留参数/机制，内容为空——JPOV 不提供隐式默认字体，
    // 用户必须通过 JPOV::Config::fonts 显式声明每个字体的 path/alias/ttc_index）。
    // 注意：stb_truetype 对 CFF (PostScript outline) 格式支持不稳定，
    // 仅加载 TrueType outline (.ttf/.ttc) 字体。
    const std::vector<std::tuple<const char*, int, const char*>> kDefaultFonts = {};

    // 字形放大倍数的合法区间由内置点阵字体决定（1 = 8x8 原生，上限 8 倍）。
    CHECK_GE(config_.debug_overlay_scale, jpov::DebugOverlayRenderer::kGlyphScaleMin)
        << "Config::debug_overlay_scale 太小";
    CHECK_LE(config_.debug_overlay_scale, jpov::DebugOverlayRenderer::kGlyphScaleMax)
        << "Config::debug_overlay_scale 太大";

    renderer_ = std::make_unique<jpov::Renderer>();
    renderer_->Init(font_tuples, kDefaultFonts, config_.shadow);

    // 采一次 GPU/驱动信息，存成引擎自己的状态（此后不变）。
    // 这是**“卡不卡”的第一信息**：WSLg / 容器里很容易静默落到 llvmpipe（纯软
    // 光栅）—— 此时再优化 shader 也只能改善一部分，真正的差别在有没有硬件光栅。
    // kind == kCpuRaster 就是软件在跑。
    debug_.gpu = jpov::QueryGpuInfo();

    initialized_ = true;
    LOG(INFO) << "JPOV::Init() — " << (config_.headless ? "headless" : "windowed")
              << " " << win_w << "x" << win_h;
    LOG(INFO) << "GL: " << jpov::GpuKindName(debug_.gpu.kind)
              << " | renderer=" << (debug_.gpu.renderer.empty() ? "?" : debug_.gpu.renderer)
              << " | vendor=" << (debug_.gpu.vendor.empty() ? "?" : debug_.gpu.vendor)
              << " | version=" << (debug_.gpu.version.empty() ? "?" : debug_.gpu.version)
              << " | max_vertex_attribs=" << debug_.gpu.max_vertex_attribs;
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
        // ⚠️ 必须把 Init() 里 setenv 的 DISPLAY 撤掉：否则同一进程再 Init() 时
        // 会走「DISPLAY 已设置 → 重试 glfwInit」分支，而那个 Xvfb 已经被我们杀了
        // ⇒ 重试必然失败并 LOG(FATAL)。（一个进程里渲两张 gold 就会踩到。）
        unsetenv("DISPLAY");
        LOG(INFO) << "JPOV: Xvfb terminated (DISPLAY unset)";
    }
#endif

    initialized_ = false;
    LOG(INFO) << "JPOV::Finalize()";
}

// ========== 纹理管理 ==========

uint32_t JPOV::RegisterTexture(const std::string& filepath) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(!filepath.empty()) << "RegisterTexture: filepath is empty";
    CHECK(renderer_ != nullptr);
    return renderer_->GetTextureManager().LoadFromFile(filepath);
}

uint32_t JPOV::RegisterTexture(const std::string& filepath,
                               const jpov::TextureOptions& opts) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(!filepath.empty()) << "RegisterTexture: filepath is empty";
    CHECK(renderer_ != nullptr);
    return renderer_->GetTextureManager().LoadFromFile(filepath, opts);
}

uint32_t JPOV::RegisterTexture(uint32_t gl_texture_id, int width, int height) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK_NE(gl_texture_id, 0u);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK(renderer_ != nullptr);
    return renderer_->GetTextureManager().Register(gl_texture_id, width, height);
}

uint32_t JPOV::RegisterTextureFromMemory(const std::vector<unsigned char>& encoded,
                                         const std::string& key,
                                         const jpov::TextureOptions& opts) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(!encoded.empty()) << "RegisterTextureFromMemory: encoded 为空";
    CHECK(!key.empty()) << "RegisterTextureFromMemory: key 为空";
    CHECK(renderer_ != nullptr);
    return renderer_->GetTextureManager().FromMemory(encoded, key, opts);
}

void JPOV::ReleaseTexture(uint32_t texture_id) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK_GT(texture_id, 0u);
    CHECK(renderer_ != nullptr);
    renderer_->GetTextureManager().Release(texture_id);
}

uint32_t JPOV::RegisterVolumeTexture(const std::string& filepath) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(!filepath.empty()) << "RegisterVolumeTexture: filepath is empty";
    CHECK(renderer_ != nullptr);
    return renderer_->GetVolumeManager().LoadFromFile(filepath);
}

int JPOV::VolumeFrameCount(uint32_t volume_id) const {
    if (renderer_ == nullptr) {
        return 0;
    }
    return renderer_->VolumeFrameCount(volume_id);
}

void JPOV::SetVolumeDebugDump(const std::string& dir) {
    if (renderer_ != nullptr) {
        renderer_->SetVolumeDebugDump(dir);
    }
}

// ========== 网格管理 ==========

uint32_t JPOV::RegisterMesh(const jpov::MeshData& data) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(renderer_ != nullptr);
    data.Validate();
    return renderer_->GetMeshManager().RegisterMesh(data);
}

uint32_t JPOV::RegisterSkeleton(
    const jpov::SkeletonType& type, std::vector<jpov::SkeletonPose> poses,
    std::array<std::vector<int>, jpov::kNumThicknessGroup> thickness_scaling_config,
    std::array<std::string, jpov::kNumPartialRotation> partial_rotation_config) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(renderer_ != nullptr);
    return renderer_->RegisterSkeleton(type, std::move(poses),
                                       std::move(thickness_scaling_config),
                                       std::move(partial_rotation_config));
}

void JPOV::UpdateMesh(uint32_t mesh_id, const jpov::MeshData& new_data) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK_GT(mesh_id, 0u);
    CHECK(renderer_ != nullptr);
    new_data.Validate();
    renderer_->GetMeshManager().UpdateMesh(mesh_id, new_data);
}

void JPOV::ReleaseMesh(uint32_t mesh_id) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK_GT(mesh_id, 0u);
    CHECK(renderer_ != nullptr);
    renderer_->GetMeshManager().ReleaseMesh(mesh_id);
}

void JPOV::ReleaseSkeleton(uint32_t skeleton_id) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(renderer_ != nullptr);
    if (skeleton_id == 0) {
        return;  // 0 = "无骨架"，静默忽略（无骨架是合法常态，不同于 ReleaseMesh 的 mesh_id>0）
    }
    renderer_->ReleaseSkeleton(skeleton_id);
}

// ========== glTF 模型加载 ==========

jpov::GltfObject JPOV::LoadGltf(const std::string& path,
                                const jpov::GltfLoadOptions& options) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(!path.empty());
    CHECK(renderer_ != nullptr);
    return renderer_->LoadGltf(path, options);
}

void JPOV::ReleaseGltf(const jpov::GltfObject& gltf) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(renderer_ != nullptr);
    renderer_->ReleaseGltf(gltf);
}

// ========== 文本宽度测量（UI 光标等需要与渲染层一致宽度） ==========

float JPOV::MeasureTextWidth(const std::string& alias, const std::string& text,
                             float font_size) {
    CHECK(initialized_) << "JPOV not initialized. Call Init() first.";
    CHECK(renderer_ != nullptr);
    return renderer_->MeasureTextWidth(alias, text, font_size);
}

// ========== 核心渲染步骤 ==========

void JPOV::RunOnceInternal(int64_t frame_count,
                            const jpov::InputSnapshot& input,
                            const jpov::WindowInfo& winfo) {
    CHECK(initialized_);

    jpov::RenderCommandList cmds;
    OneIteration(frame_count, input, winfo, &cmds);

    // ---- 更新 debug 状态（引擎自己的状态，不进 RenderCommandList）----
    debug_.camera      = cmds.camera;
    debug_.width       = winfo.width;
    debug_.height      = winfo.height;
    debug_.frame_count = frame_count;
    {
        // 瞬时 fps = 1 / 相邻两帧的墙钟间隔（含渲染耗时，因此是真帧率）。
        // 首帧没有上一帧参照 ⇒ 保持 0。
        const double now = glfwGetTime();
        if (last_frame_time_ > 0.0) {
            const double dt = now - last_frame_time_;
            debug_.fps = (dt > 0.0) ? static_cast<float>(1.0 / dt) : 0.0f;
        }
        last_frame_time_ = now;
    }

    renderer_->BeginFrame(
        static_cast<int>(winfo.width),
        static_cast<int>(winfo.height));
    renderer_->Render(cmds, winfo);

    // ---- debug HUD：必须在 Render() 之后画（落在主 FBO 最上层，
    //      Present 与 SaveScreenshot 都会带上它）----
    if (debug_overlay_visible_) {
        renderer_->DrawDebugOverlay(debug_.DebugString(), config_.debug_overlay_scale);
    }
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

void JPOV::RunOnce(int64_t frame_count, const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    const char* out_png_path) {
    CHECK(initialized_);
    CHECK_GE(frame_count, 0);
    CHECK_GT(winfo.width, 0);
    CHECK_GT(winfo.height, 0);
    CHECK(out_png_path != nullptr);

    // 1. 核心渲染（与 Run() 完全一致）
    RunOnceInternal(frame_count, input, winfo);

    // 2. 保存窗口尺寸截图
    int win_w = static_cast<int>(winfo.width);
    int win_h = static_cast<int>(winfo.height);
    renderer_->SaveScreenshot(win_w, win_h, out_png_path);
}

void JPOV::RunOnce(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    const char* out_png_path) {
    RunOnce(/*frame_count=*/0, input, winfo, out_png_path);
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

void JPOV::HandleKey(int key, int, int action, int mods) {
    // 修饰键状态：每次按键事件都从 mods 位掩码刷新（GLFW 在事件里携带当前
    // 修饰键快照，shift 为左右任一按下）。文本输入框用它决定上档符号/大小写。
    mods_.shift = (mods & GLFW_MOD_SHIFT) != 0;
    mods_.ctrl  = (mods & GLFW_MOD_CONTROL) != 0;
    mods_.alt   = (mods & GLFW_MOD_ALT) != 0;
    mods_.super = (mods & GLFW_MOD_SUPER) != 0;

    if (key < 0 || key >= jpov::kMaxKeyCode) {
        return;
    }

    // `~`（美式键盘的 grave accent 键，96）是**引擎保留键**：切换左上角 debug HUD。
    // 在这里 return，不再写进 keys_ —— 即应用在 InputSnapshot 里**看不到**这次按键。
    // 只在按下时切换（REPEAT 不重复切）。
    //
    // ⚠️ 代价（有意为之，不是遗忘）：ui.cc / sp_ui.cc 的文本框靠 KeyCode::GraveAccent
    // 输入 '`' 与 '~'，引擎接管后**文本框就再也输不了这两个字符**。这与 Quake
    // 系控制台键的传统一致；若将来某个 app 必须文本输入反引号，需另给一个开关。
    if (key == static_cast<int>(jpov::KeyCode::GraveAccent) && action == GLFW_PRESS) {
        debug_overlay_visible_ = !debug_overlay_visible_;
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
    input->mods = mods_;
}

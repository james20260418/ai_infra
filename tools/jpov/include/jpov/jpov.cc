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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>

#if defined(_WIN32)
// windows.h 定义 ERROR 宏，与 glog 的 ERROR 严重级别冲突 ⇒ 必须先 suppress（同
// shader_manager.cc）。本文件只需 GetModuleFileNameA / MAX_PATH。
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include <windows.h>
#include <io.h>
#include <timeapi.h>   // timeBeginPeriod：把系统计时器粒度压到 1ms（帧率控制需要）
// winuser.h 把 DrawText 定义成 DrawTextA（Win32 GDI 的绘制文本 API），会撞上
// RenderCommandList::DrawText。本文件不用 GDI 画字，撤掉这个宏。
#ifdef DrawText
#undef DrawText
#endif
#endif

#include <glog/logging.h>

#include "tools/jpov/src/debug_overlay/debug_overlay_renderer.h"
#include "tools/jpov/src/gpu_info_query.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
// ---- 混合显卡笔记本的「优先独显」提示（唯一官方手段）----
//
// Windows + OpenGL 没有选设备的 API（见 interface/gpu_info.h 顶部注释），能让驱动
// 优先挑独显的只有这两个**全局变量**：最终 exe 的导出表里存在它们时，NVIDIA / AMD
// 驱动在创建 context 时会优先使用高性能 GPU。
//
// 两个约束决定了它们必须写在这里、而不是某个按需链接的模块里：
//   ① 必须是 __declspec(dllexport) —— 驱动直接读导出表；
//   ② 必须落在**一定会被链接进 exe 的 TU** —— 静态库里没人引用的目标文件会被丢弃，
//      导出符号会随之消失（jpov.cc 恒被链接，所以放在这里）。
//
// 注意：导出表是链接期的静态属性，运行时无法按 Config::render_preference 开关 ——
// 所以它对所有 JPOV app 都生效。用户在「设置 → 显示 → 图形」或显卡控制面板里
// 为本 exe 手动指定时，玩家的选择赢（这也是游戏引擎的通行做法）。
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
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

namespace {

// 睡到绝对时刻 deadline（秒，glfwGetTime 基准）。
//
// 为什么不直接 nanosleep「剩余时长」：Windows 上 Sleep/nanosleep 的粒度默认是
// ~15.6ms，睡 11ms 可能真睡 16ms —— 60 帧的目标直接被压成 ~46 帧（实测）。
// 这里两级：先粗睡到 deadline - 1.5ms，再自旋收尾（自旋只占 1.5ms/帧，代价可控）。
// spin_tail=false 时不自旋：粗睡到 deadline 就返回（开 vsync 时够用，见 Run()）。
void SleepUntilSeconds(double deadline, bool spin_tail) {
    // spin_tail：是否用「自旋收尾」把误差压到亚毫秒。
    //   · 关 vsync ⇒ 需要精确（睡早/睡晚都直接变成帧率误差）；
    //   · 开 vsync ⇒ 只要**提前一点**到达 swap 即可（多等的那几毫秒由垂直同步吃掉），
    //     此时自旋纯属烧 CPU（1.5ms/帧 ≈ 一个核的 9%），所以用粗睡。
    const double kSpinWindow = spin_tail ? 0.0015 : 0.0;
    for (;;) {
        const double remaining = deadline - glfwGetTime();
        if (remaining <= 0.0) {
            return;
        }
        if (remaining > kSpinWindow) {
            const double coarse = remaining - kSpinWindow;
            const long us = static_cast<long>(coarse * 1e6);
            if (us > 0) {
                struct timespec ts = {0, us * 1000};
                nanosleep(&ts, nullptr);
            }
        }
        // 尾部自旋：不再 sleep，避免被系统调度走导致睡过头。
    }
}

// 相对日志路径 → exe 所在目录下的路径（Windows 双击启动时 CWD 不保证是 exe 目录）。
// 绝对路径原样返回。
std::string ResolveLogPath(const char* log_file) {
    std::string path(log_file);
#if defined(_WIN32)
    // "C:\\..." / "\\\\server\\share..." 视为绝对路径，其余按 exe 目录解析。
    const bool is_absolute =
        (path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
        (path.size() >= 2 && path[0] == '\\' && path[1] == '\\');
    if (!is_absolute) {
        char exe_path[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0) {
            std::string exe(exe_path);
            const size_t slash = exe.find_last_of("\\/");
            if (slash != std::string::npos) {
                path = exe.substr(0, slash + 1) + path;
            }
        }
    }
#endif
    return path;
}

// Config::log_file 非空时把 stderr 重定向到该文件。
//
// 为什么用「重定向 stderr」而不是给日志加一个文件 sink：glog 默认就写 stderr，
// 重定向是唯一一处改动、覆盖所有日志来源（含第三方库与崩溃时的 CHECK 输出）。
// 无缓冲是必须的：LOG(FATAL) 之后进程立刻 abort，缓冲区里没刷出去的行就永远丢了。
void RedirectStderrToLogFile(const char* log_file) {
    // 默认名带时间戳：双击跑好几次也不会互相覆盖（出事时拿最近那份就是）。
    std::string auto_name;
    const char* chosen = log_file;
#if defined(_WIN32)
    if (chosen == nullptr) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char name[64];
        std::snprintf(name, sizeof(name), "jpov_%04u%02u%02u-%02u%02u%02u_%lu_log.txt",
                      static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                      static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                      static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        auto_name = name;
        chosen = auto_name.c_str();
    }
#else
    if (chosen == nullptr) {
        return;   // 其他平台默认不落盘（CI 不该到处留文件）
    }
#endif
    if (chosen[0] == '\0') {
        return;
    }
    std::string path = ResolveLogPath(chosen);
    FILE* file = std::fopen(path.c_str(), "w");
#if defined(_WIN32)
    // exe 放在 \\wsl$ / 网络共享 / 只读目录时这里打不开（实测：从 WSL 盘直接运行）
    // ⇒ 退到 %TEMP%：总比完全没有日志强，实际路径会打在日志第一行。
    if (file == nullptr) {
        char temp_dir[MAX_PATH] = {0};
        if (GetTempPathA(MAX_PATH, temp_dir) > 0) {
            std::string fallback(temp_dir);
            fallback += "jpov_run_log.txt";
            FILE* alt = std::fopen(fallback.c_str(), "w");
            if (alt != nullptr) {
                path = fallback;
                file = alt;
            }
        }
    }
#endif
    if (file == nullptr) {
        LOG(WARNING) << "日志文件打开失败：" << path << "（日志仍然只写 stderr）";
        return;
    }
    std::setvbuf(file, nullptr, _IONBF, 0);
#if defined(_WIN32)
    _dup2(_fileno(file), _fileno(stderr));
#else
    dup2(fileno(file), fileno(stderr));
#endif
    LOG(INFO) << "日志已重定向到 " << path;
}

#if defined(_WIN32)
// 未捕获异常兜底：把异常码 / 出错地址 / 模块内偏移 / 调用栈写进日志（= 日志文件）。
//
// 为什么必需：真正的崩溃（访问违例 / 除零）不是 LOG(FATAL)，一行日志都不会留；
// 双击运行时控制台窗口又立刻消失 ⇒ 没有这个 handler 就完全无从下手。
// 输出的 rva（相对 exe 镜像基址的偏移）可以拿二进制反查：
//   x86_64-w64-mingw32-addr2line -e <exe> -f -C 0x<rva>
// 崩溃路径上的日志输出用**裸 fd 写**（_write(2, …)）而不是 fprintf：
//   · 崩溃可能就发生在 stdio 内部（比如 glog 正在写日志）—— 那时 stderr 的
//     FILE 锁可能还握着，fprintf 会直接死锁，什么也拿不到；
//   · _write 不做缓冲、不加锁，且 stderr 已被重定向到日志文件。
void CrashLog(const char* text) {
    const int fd = _fileno(stderr);
    if (fd < 0) {
        return;
    }
    _write(fd, text, static_cast<unsigned>(std::strlen(text)));
}

LONG WINAPI JpovUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = (info != nullptr) ? info->ExceptionRecord : nullptr;
    const unsigned long long module_base =
        reinterpret_cast<unsigned long long>(GetModuleHandleA(nullptr));
    char line[256];
    CrashLog("\n*** FATAL: 未捕获异常 ***\n");
    if (rec != nullptr) {
        const unsigned long long addr =
            reinterpret_cast<unsigned long long>(rec->ExceptionAddress);
        std::snprintf(line, sizeof(line), "exception code = 0x%08lX\n",
                      static_cast<unsigned long>(rec->ExceptionCode));
        CrashLog(line);
        std::snprintf(line, sizeof(line),
                      "fault address  = 0x%llX  (exe rva = 0x%llX)\n",
                      addr, (module_base != 0) ? (addr - module_base) : 0ULL);
        CrashLog(line);
    }
    // 崩在哪儿：ContextRecord->Rip 是**真正**出错的那条指令（调用空指针时是 0）。
    if (info != nullptr && info->ContextRecord != nullptr) {
        const unsigned long long rip =
            static_cast<unsigned long long>(info->ContextRecord->Rip);
        std::snprintf(line, sizeof(line), "rip            = 0x%llX  (exe rva = 0x%llX)\n",
                      rip, (module_base != 0) ? (rip - module_base) : 0ULL);
        CrashLog(line);
        // 栈扫描：CaptureStackBackTrace 只能看到 handler 自己的栈，对崩溃上下文没用
        //（实测帧全是系统 DLL）。没有 dbghelp 符号时，从 Rsp 往上翻「落在本 exe
        // 镜像内、且像返回地址」的值，是最直接能拿到调用点的办法。
        const unsigned char* mod = reinterpret_cast<const unsigned char*>(
            GetModuleHandleA(nullptr));
        unsigned long long mod_size = 0;
        if (mod != nullptr) {
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
            const IMAGE_NT_HEADERS64* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(mod + dos->e_lfanew);
            mod_size = nt->OptionalHeader.SizeOfImage;
        }
        const unsigned long long* sp =
            reinterpret_cast<const unsigned long long*>(info->ContextRecord->Rsp);
        int shown = 0;
        for (int i = 0; i < 512 && shown < 16; ++i) {
            const unsigned long long v = sp[i];
            if (v >= module_base && v < module_base + mod_size) {
                std::snprintf(line, sizeof(line), "  stack[%3d] rva = 0x%llX\n",
                              i, v - module_base);
                CrashLog(line);
                ++shown;
            }
        }
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// 偏好落空时的告警：必须写「为什么 + 怎么办」，否则用户只会看到一个慢得很均匀的
// 窗口，无从下手。
// Pre-condition: gpu 是本次 Init 里 QueryGpuInfo() 的快照（context 已创建）。
void ReportRenderPreference(jpov::RenderPreference preference,
                            const jpov::GpuInfo& gpu) {
    const jpov::RenderPreferenceVerdict verdict =
        jpov::JudgeRenderPreference(preference, gpu.kind);
    if (verdict == jpov::RenderPreferenceVerdict::kSatisfied) {
        return;
    }
    // kUnknown 时冤枉告警比不告警更糟：glGetString 什么都没给，我们不知道真相。
    if (verdict == jpov::RenderPreferenceVerdict::kUndetermined) {
        LOG(WARNING) << "Config::render_preference="
                     << jpov::RenderPreferenceName(preference)
                     << " 无法核对：GL_RENDERER 为空（GpuKind=kUnknown）";
        return;
    }
    const char* renderer = gpu.renderer.empty() ? "?" : gpu.renderer.c_str();
    if (preference == jpov::RenderPreference::kPreferGpu) {
        LOG(WARNING)
            << "Config::render_preference=prefer-gpu，但当前 context 落在软件光栅器上"
               "（renderer=" << renderer << "）。JPOV/OpenGL 无法自行选设备，请排查："
               "① 是否在 WSLg / 远程桌面 / 虚拟机里（这些环境常常没有硬件 GL）；"
               "② 显卡驱动是否正常安装；"
               "③ Windows「设置 → 系统 → 显示 → 图形」里为本 exe 选「高性能」；"
               "④ 笔记本混合显卡用 NVIDIA 控制面板 / AMD 软件做 per-app 首选项。"
               "不中断：偏好落空只影响帧率，不影响正确性。";
        return;
    }
    LOG(WARNING)
        << "Config::render_preference=prefer-cpu，但当前 context 不是已知软件光栅器"
           "（renderer=" << renderer << "）。LIBGL_ALWAYS_SOFTWARE 只对 Mesa 系驱动"
           "生效；Windows 上「系统 opengl32.dll + 厂商 ICD」那条栈没有软件后端，"
           "需要把 Mesa 的 opengl32.dll 放到 exe 旁才可能落软光栅。";
}

}  // namespace

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

    // 日志落盘必须排在所有 LOG 之前：Windows 双击运行时没有可看的控制台，
    // 崩溃（LOG(FATAL)/CHECK）时控制台窗口会立刻关闭，后面这些行就是唯一线索。
    RedirectStderrToLogFile(config_.log_file);
#if defined(_WIN32)
    // 紧挨着装：Init 内部也可能崩（GL context / 着色器 / 字体）。
    SetUnhandledExceptionFilter(JpovUnhandledExceptionFilter);
#endif
    // 非法枚举值（外部 cast 出来的）会让后面的 switch 无从判断，早崩早暴露。
    CHECK(config_.render_preference == jpov::RenderPreference::kAny ||
          config_.render_preference == jpov::RenderPreference::kPreferGpu ||
          config_.render_preference == jpov::RenderPreference::kPreferCpu)
        << "Config::render_preference 非法：不是 RenderPreference 的枚举值";

    // 偏好必须在**建 context 之前**落地：Mesa 只在建 context 时读这个环境变量。
    if (jpov::ApplyRenderPreferenceEnvHint(config_.render_preference)) {
        LOG(INFO) << "Config::render_preference=prefer-cpu ⇒ 已设置 "
                     "LIBGL_ALWAYS_SOFTWARE=1（Mesa 系驱动会在建 context 时落软光栅）";
    }

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

    // 垂直同步：显式设置（GLFW 默认就是 1，但写出来才能和 Config::vsync 对上）。
    glfwSwapInterval(config_.vsync ? 1 : 0);
#if defined(_WIN32)
    // 帧率控制依赖 sleep 精度：默认系统计时器粒度 ~15.6ms，会把 60 帧压到 ~46。
    // 压到 1ms 后配 SleepUntilSeconds 的 1.5ms 自旋尾巴，误差 <0.5ms。
    timeBeginPeriod(1);
    timer_period_active_ = true;
#endif

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
    // 注意：follow_dpi = true 时这个字段只是"万一要覆盖"的备用值，仍然校验。
    CHECK_GE(config_.debug_overlay_scale, jpov::DebugOverlayRenderer::kGlyphScaleMin)
        << "Config::debug_overlay_scale 太小";
    CHECK_LE(config_.debug_overlay_scale, jpov::DebugOverlayRenderer::kGlyphScaleMax)
        << "Config::debug_overlay_scale 太大";

    // debug HUD 实际字号：默认跟随窗口 DPI 缩放（Windows 200% / macOS Retina 上
    // content scale = 2）。这样"看起来"永远是 8 逻辑像素 —— 100% 屏结果与固定
    // 8 物理像素完全一致，200% 屏才不会缩成 4 个逻辑像素看不清。
    // 注意这是 Init 时的快照：GLFW 不做运行中 DPI 迁移，运行期不会变。
    debug_overlay_scale_ = config_.debug_overlay_scale;
    if (config_.debug_overlay_scale_follow_dpi) {
        float sx = 1.0f;
        float sy = 1.0f;
        glfwGetWindowContentScale(window_, &sx, &sy);
        const float dpi_scale = std::max(sx, sy);
        int rounded = static_cast<int>(dpi_scale + 0.5f);   // 四舍五入，1.5 -> 2
        rounded = std::max(rounded, jpov::DebugOverlayRenderer::kGlyphScaleMin);
        rounded = std::min(rounded, jpov::DebugOverlayRenderer::kGlyphScaleMax);
        debug_overlay_scale_ = rounded;
    }

    renderer_ = std::make_unique<jpov::Renderer>();
    renderer_->Init(font_tuples, kDefaultFonts, config_.shadow);

    // 采一次 GPU/驱动信息，存成引擎自己的状态（此后不变）。
    // 这是**“卡不卡”的第一信息**：WSLg / 容器里很容易静默落到 llvmpipe（纯软
    // 光栅）—— 此时再优化 shader 也只能改善一部分，真正的差别在有没有硬件光栅。
    // kind == kCpuRaster 就是软件在跑。
    debug_.gpu = jpov::QueryGpuInfo();

    // 偏好核对：用 glGetString 的真话判定；不满足只告警（“偏好”不是“要求”）。
    ReportRenderPreference(config_.render_preference, debug_.gpu);

    // 先把窗口变成"有内容"再返回：应用随后要加载模型/贴图，期间窗口停在这一帧。
    if (!config_.headless) {
        DrawSplashFrame(win_w, win_h);
    }

    initialized_ = true;
    LOG(INFO) << "JPOV::Init() — " << (config_.headless ? "headless" : "windowed")
              << " " << win_w << "x" << win_h;
    LOG(INFO) << "GL: " << jpov::GpuKindName(debug_.gpu.kind)
              << " | renderer=" << (debug_.gpu.renderer.empty() ? "?" : debug_.gpu.renderer)
              << " | vendor=" << (debug_.gpu.vendor.empty() ? "?" : debug_.gpu.vendor)
              << " | version=" << (debug_.gpu.version.empty() ? "?" : debug_.gpu.version)
              << " | max_vertex_attribs=" << debug_.gpu.max_vertex_attribs
              << " | preference=" << jpov::RenderPreferenceName(config_.render_preference);
}

void JPOV::Finalize() {
#if defined(_WIN32)
    if (timer_period_active_) {
        timeEndPeriod(1);
        timer_period_active_ = false;
    }
#endif
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

const char* JPOV::GetClipboardText() const {
    // 无窗口（headless 拍摄 / 未 Init）时剪贴板不存在：返回 nullptr 而不是空串，
    // 让调用方（UI）能把“没有剪贴板”与“剪贴板是空的”区分开（两者对粘贴都是无内容）。
    if (window_ == nullptr) {
        return nullptr;
    }
    return glfwGetClipboardString(window_);
}

void JPOV::SetClipboardText(const char* text) {
    CHECK(text != nullptr) << "SetClipboardText: text must not be null";
    if (window_ == nullptr) {
        return;  // headless：无窗口可写，静默跳过（测试/拍摄不会崩）。
    }
    glfwSetClipboardString(window_, text);
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
        // fps：每 0.5 秒统计一次，结果保持到下一个统计窗口结束（读得出来、可比）。
        // 首个窗口结束前 has_result() 为 false ⇒ 读数保持 0。
        const double now = glfwGetTime();
        fps_stat_.Tick(now);
        if (fps_stat_.has_result()) {
            debug_.fps = static_cast<float>(fps_stat_.rate_per_second());
        }
    }

    renderer_->BeginFrame(
        static_cast<int>(winfo.width),
        static_cast<int>(winfo.height));
    renderer_->Render(cmds, winfo);

    // ---- debug HUD：必须在 Render() 之后画（落在主 FBO 最上层，
    //      Present 与 SaveScreenshot 都会带上它）----
    if (debug_overlay_visible_) {
        renderer_->DrawDebugOverlay(debug_.DebugString(), debug_overlay_scale_);
    }
}


void JPOV::DrawSplashFrame(int width, int height) {
    CHECK(renderer_ != nullptr);
    CHECK(window_ != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    jpov::RenderCommandList cmds;
    cmds.Clear();
    // 深灰底：比纯黑更像"已经起来了"，也不会被误认成白屏/花屏故障。
    const jpov::Color background = {0.10f, 0.11f, 0.13f, 1.0f};
    cmds.DrawRect({0.0f, 0.0f},
                  {static_cast<float>(width), static_cast<float>(height)}, background);
    if (!config_.fonts.empty()) {
        // 只画 ASCII：任何注册字体都含 ASCII 字形，而 CJK 字体不一定被注册。
        const char* kSplashText = "JPOV - starting...";
        const float font_size = 20.0f;
        cmds.DrawText(kSplashText,
                      {static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f},
                      font_size, jpov::kColorWhite,
                      /*alignment=*/jpov::TextAlignment::kCenter,
                      /*font_alias=*/config_.fonts.front().alias);
    }
    const jpov::WindowInfo winfo = {static_cast<float>(width), static_cast<float>(height)};
    renderer_->BeginFrame(width, height);
    renderer_->Render(cmds, winfo);
    renderer_->Present(window_, width, height);
    LOG(INFO) << "启动画面已绘制（加载资源期间窗口保持这一帧）";
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
        //
        // 睡到「本帧开始 + 目标间隔」这个**绝对时刻**（不是「睡剩余时长」，两者在
        // 有抖动时会越差越多）。
        //
        // ⚠️ 开了 vsync 时必须**提前几毫秒**到达 swap（这里取 3ms）：swap 会阻塞到下一个垂直同步，
        // 如果我们正好睡到 16.67ms 才去 swap，就会错过最近的那次刷新、白等到再下
        // 一次 —— 实测把 60Hz 屏上的 60 帧压成 ~46 帧（2026-11-01 真卡）。
        // 关掉 vsync 时则严格睡到 deadline（此时 swap 不阻塞，睡早就是白丢帧）。
        if (config_.target_fps > 0) {
            // 提前 3ms 到达 swap：留给系统调度抖动一点余量（偶尔被抢占 2~3ms 也不会
            // 错过垂直同步；实测提前 2ms 时仍会零星掉到 ~57 帧）。
            constexpr double kVsyncArriveEarly = 0.003;
            const double deadline = frame_start_time_ + frame_interval -
                                    (config_.vsync ? kVsyncArriveEarly : 0.0);
            SleepUntilSeconds(deadline, /*spin_tail=*/!config_.vsync);
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
    // 左键按下的起点（仅左键；本帧按住时有效，松开后是残留值）。
    input->left_press_x = static_cast<float>(left_btn_.press_x);
    input->left_press_y = static_cast<float>(left_btn_.press_y);
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
        slot.s->press_x = mouse_x_;
        slot.s->press_y = mouse_y_;
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

    // `F3` 是**引擎保留键**：切换左上角 debug HUD。
    // 在这里 return，不再写进 keys_ —— 即应用在 InputSnapshot 里**看不到**这次按键。
    // 只在按下时切换（REPEAT 不重复切）。
    //
    // 为什么不是 Quake 系的 `~`：那个键会把 grave accent 从 InputSnapshot 里吞掉，
    // ui.cc / sp_ui.cc 的文本框**就再也输不了 '`' 和 '~'**。功能键不进任何文本
    // 输入路径，代价为零；F3 = debug 面板也是「类似 Minecraft 的独立游戏」
    // 这个目标体量的通行惯例。
    if (key == static_cast<int>(jpov::KeyCode::F3) && action == GLFW_PRESS) {
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

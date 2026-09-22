// JPOV/AI Infra 通用工具函数
//
// 提供项目级别的辅助函数，供各模块复用。

#ifndef TOOLS_COMMON_UTILS_H_
#define TOOLS_COMMON_UTILS_H_

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace jpov {

// 返回工程根目录绝对路径（末尾带 '/'）。
//
// 路径来源优先级：
//   1. BUILD_WORKSPACE_DIRECTORY（bazel run 自动设置）
//   2. PROJECT_ROOT 环境变量
//   3. 当前工作目录
inline std::string GetProjectRoot() {
    const char* env = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    if (!env) {
        env = std::getenv("PROJECT_ROOT");
    }
    if (!env) {
        env = ".";
    }
    std::string root(env);
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    return root;
}

// 返回项目统一的输出目录绝对路径，格式为 "<工程根目录>/output/"。
//
// 所有生成文件（截图、日志等）应写入此目录下的子目录。
inline std::string GetOutputDir() {
    return GetProjectRoot() + "output/";
}

// 返回当前可执行文件所在目录（绝对路径，末尾不带 '/'）。
//
// 用于**分发态资源定位**：exe 旁的资源相对路径（如 `fonts/x.ttf`、`models/x.glb`）
// 应相对于 exe 目录解析，而不是相对于启动时的 cwd —— 否则从任意目录启动都找不到。
// - Linux: 读 /proc/self/exe 符号链接
// - Windows: GetModuleFileNameA(NULL, ...)
// 获取失败返回空串（表示无法定位，调用方回退到其它路径策略）。
inline std::string GetExeDir() {
#ifdef _WIN32
    char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return "";
    }
    const std::string p(buf, n);
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? "" : p.substr(0, slash);
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return "";
    }
    buf[n] = '\0';
    const std::string p(buf);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? "" : p.substr(0, slash);
#endif
}

// 解析外部资源路径（glTF / 贴图 / 字体等），返回实际可读路径。
//
// 查找顺序（与 PR #68 定下的字体路径纪律一致，跨平台、无硬编码目录约定）：
//   1. **exe 旁边**的原始相对路径 —— 分发态（`output/<demo>/<raw_path>`）
//      （仅相对路径适用；绝对路径拼 exe 目录无意义）
//   2. 原始路径 —— 绝对路径按原样，相对路径按**当前工作目录**
//      （开发态：`bazel run` 的 cwd = 工程根）
//   3. `$TEST_SRCDIR/__main__/<raw_path>` —— bazel test 沙箱
// 全部未命中时返回**原路径原样**（让调用方的 loader 报出它自己的错误信息，
// 而不是在这里静默返回空）。
//
// 收 `std::string` 而非 `const char*`：调用方常用 std::string 拼接后传入。
// Pre-condition: 无（空串原样返回）。
inline std::string ResolveResourcePath(const std::string& raw_path) {
    if (raw_path.empty()) {
        return raw_path;
    }
    // 判断是否相对路径（不以 '/' 或 '\\' 开头，且无 Windows 盘符）。
    const bool is_relative =
        raw_path[0] != '/' && raw_path[0] != '\\' &&
        !(raw_path.size() >= 2 && raw_path[1] == ':');

    auto readable = [](const std::string& p) {
        FILE* fp = std::fopen(p.c_str(), "rb");
        if (fp == nullptr) {
            return false;
        }
        std::fclose(fp);
        return true;
    };

    // 1. 分发态：exe 旁相对路径。
    if (is_relative) {
        const std::string exe_dir = GetExeDir();
        if (!exe_dir.empty()) {
            const std::string dist = exe_dir + "/" + raw_path;
            if (readable(dist)) {
                return dist;
            }
        }
    }
    // 2. 开发态：原路径（绝对按原样 / 相对按 cwd）。
    if (readable(raw_path)) {
        return raw_path;
    }
    // 3. bazel test 沙箱。
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir != nullptr) {
        std::string p(srcdir);
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        p += "__main__/";
        p += raw_path;
        if (readable(p)) {
            return p;
        }
    }
    // 未命中：原样返回，交给调用方的 loader 报错。
    return raw_path;
}

// 生成"编辑时间戳后缀"字符串：`YYYYMMDD-HHMMSS`（本地时间）。
//
// 用途：模型编辑器保存按钮拼文件名（`<stem>_edit<后缀>.glb`）。放在公共位置是因为
//   "日期时间字符串"是跨模块的通用需求，不该在 demo/editor 里私有实现。
//
// 收 `now`（std::time_t）而非内部取当前时间 —— 便于单测固定输入、结果可复现。
// Pre-condition: now 为有效 time_t（正常调用传 std::time(nullptr)）。
inline std::string EditTimestampSuffix(std::time_t now) {
    std::tm tm_buf{};
    // localtime_r：线程安全版本（保存线程里调用，不能用非线程安全的 localtime）。
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
    return std::string(buf);
}

}  // namespace jpov

#endif  // TOOLS_COMMON_UTILS_H_

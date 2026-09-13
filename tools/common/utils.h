// JPOV/AI Infra 通用工具函数
//
// 提供项目级别的辅助函数，供各模块复用。

#ifndef TOOLS_COMMON_UTILS_H_
#define TOOLS_COMMON_UTILS_H_

#include <cstdlib>
#include <ctime>
#include <string>

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

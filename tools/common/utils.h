// JPOV/AI Infra 通用工具函数
//
// 提供项目级别的辅助函数，供各模块复用。

#ifndef TOOLS_COMMON_UTILS_H_
#define TOOLS_COMMON_UTILS_H_

#include <cstdlib>
#include <string>

namespace jpov {

// 返回项目统一的输出目录绝对路径，格式为 "<工程根目录>/output/"。
//
// 路径来源优先级：
//   1. BUILD_WORKSPACE_DIRECTORY（bazel run 自动设置）
//   2. PROJECT_ROOT 环境变量
//   3. 当前工作目录
//
// 所有生成文件（截图、日志等）应写入此目录下的子目录。
inline std::string GetOutputDir() {
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
    return root + "output/";
}

}  // namespace jpov

#endif  // TOOLS_COMMON_UTILS_H_

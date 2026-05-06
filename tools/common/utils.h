// JPOV/AI Infra 通用工具函数
//
// 提供项目级别的辅助函数，供各模块复用。

#ifndef TOOLS_COMMON_UTILS_H_
#define TOOLS_COMMON_UTILS_H_

#include <string>

namespace jpov {

// 返回项目统一的输出目录绝对路径。
// 所有生成文件（截图、日志等）应写入此目录下的子目录。
// 当前实现硬编码为 /james/ai_infra/output/
inline std::string GetOutputDir() {
    return "/james/ai_infra/output/";
}

}  // namespace jpov

#endif  // TOOLS_COMMON_UTILS_H_

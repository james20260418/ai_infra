// JPOV Test Utilities — 共享的测试辅助函数
//
// 同目录原则：作为 BUILD 同目录下的 header-only 工具，
// gold test 通过 cc_test deps 直接引用此头文件。

#ifndef JPOV_TEST_TEST_UTILS_H_
#define JPOV_TEST_TEST_UTILS_H_

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

namespace jpov {

// 读取文件全部字节到 vector
// Returns false on failure (log error internally).
inline bool ReadFileBytes(const std::string& path,
                          std::vector<uint8_t>* out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG(ERROR) << "Failed to open file: " << path;
        return false;
    }
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(out->data()), size)) {
        LOG(ERROR) << "Failed to read file: " << path;
        return false;
    }
    return true;
}

}  // namespace jpov

#endif  // JPOV_TEST_TEST_UTILS_H_

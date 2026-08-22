// JPOV Test Utilities — shared test helper functions
//
// Header-only; gold tests include this directly or via BUILD deps.

#ifndef JPOV_TEST_TEST_UTILS_H_
#define JPOV_TEST_TEST_UTILS_H_

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"

namespace jpov {

// Returns test data directory prefix (no trailing slash):
//   bazel test sandbox: $TEST_SRCDIR/__main__/tools/jpov/test
//   bazel run / local: <project_root>/tools/jpov/test
// All generator and gold test resource paths should be constructed via
// this function, ensuring tests run after moving the repo.
inline std::string GetTestDataDir() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test";
        return p;
    }
    return GetProjectRoot() + "tools/jpov/test";
}

// Read entire file into vector.
// Returns false on failure (logs error internally).
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

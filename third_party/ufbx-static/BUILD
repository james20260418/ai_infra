# third_party/ufbx-static — 单文件 FBX 加载库（静态 vendoring）
#
# 来源：https://github.com/ufbx/ufbx （main 分支，2026-09-08 抓取）
#   ufbx.c / ufbx.h 单文件拷贝（portable C99/C++11，官方支持 Linux+GCC、MinGW 交叉）。
# License：双许可可选 —— MIT License 或 Public Domain(Unlicense)，见 ufbx.c/u.fbx.h 头部；
#   本项目按 MIT 条款使用（可商用/修改，保留版权声明）。仅 LLVM 无外部依赖，无需额外 linkopts。
#
# 用途：JPOV 骨架子系统读 FBX 骨骼动画源（Mixamo 下载的 dance 等 → skeleton_ninja 后续当驱动）。
# 与 glfw-static / curl-static 一样的 vendoring 模式；ufbx.c 编译为静态库，ofbx.h 公开。

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ufbx",
    srcs = ["ufbx.c"],
    hdrs = ["ufbx.h"],
    # ufbx.c 按 C 编译（gcc 对 .c 会使 C 默认；不要给 C++ 的 -std=c++17 以免对 C warning）。
    # ufbx 官方 Setup：ufbx.c 需 C99/C++11+，无外部 deps。
    includes = ["."],
    linkstatic = 1,
)

# 供 gen3d/骨架 smoke 或 loader 引用「编译过」的最小检查：编译库即可。
# （无需单独 test target——下游/冒烟会在骨架 loader 引入时以 cc_binary 间接覆盖。）

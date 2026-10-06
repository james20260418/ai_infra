package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# PiecewiseLinearFunction library (header-only)
cc_library(
    name = "piecewise_linear_function",
    hdrs = ["piecewise_linear_function.h"],
    deps = [
        "//geom/common:check",
    ],
    copts = ["-std=c++20"],
)

# PiecewiseLinearFunction tests
cc_test(
    name = "piecewise_linear_function_test",
    srcs = ["piecewise_linear_function_test.cc"],
    deps = [
        ":piecewise_linear_function",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# SizeLimitedPiecewiseLinearFunction — 定长分段线性函数（段数极少时用；header-only）
# 线性扫描求值 + 带 index_hint 的双指针查询 + UpProp/DownProp（立柱顶起/压下）。
cc_library(
    name = "size_limited_piecewise_linear_function",
    hdrs = ["size_limited_piecewise_linear_function.h"],
    deps = [
        "//geom/common:check",
    ],
    copts = ["-std=c++20"],
)

# SizeLimitedPiecewiseLinearFunction tests
cc_test(
    name = "size_limited_piecewise_linear_function_test",
    srcs = ["size_limited_piecewise_linear_function_test.cc"],
    deps = [
        ":size_limited_piecewise_linear_function",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# ConvexHullTiles — 屏幕空间凸包（PWL 上/下壳）→ 覆盖的栅格 tile（扫描线；context 无关）
cc_library(
    name = "convex_hull_tiles",
    hdrs = ["convex_hull_tiles.h"],
    deps = [
        "//geom/common:check",
    ],
    copts = ["-std=c++20"],
)

# ConvexHullTiles tests（含随机 8 点 cross-validate）
cc_test(
    name = "convex_hull_tiles_test",
    srcs = ["convex_hull_tiles_test.cc"],
    deps = [
        ":convex_hull_tiles",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Mat4 — 4x4 变换矩阵（列主序）+ 仿射求逆（骨架 rest/inverse-bind 用；header-only）
cc_library(
    name = "mat4",
    hdrs = ["mat4.h"],
    deps = [
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# Mat4 tests
cc_test(
    name = "mat4_test",
    srcs = ["mat4_test.cc"],
    deps = [
        ":mat4",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# DualQuat — 刚体变换的对偶四元数（DQS 蒙皮：刚体混合，保体积/无糖纸；header-only）
# 被 src/skeleton 的 atlas 烘焙（刚体矩阵 → 对偶四元数）与测试侧 CPU 真值共用。
cc_library(
    name = "dual_quat",
    hdrs = ["dual_quat.h"],
    deps = [
        ":mat4",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# DualQuat tests
cc_test(
    name = "dual_quat_test",
    srcs = ["dual_quat_test.cc"],
    deps = [
        ":dual_quat",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

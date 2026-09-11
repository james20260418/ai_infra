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

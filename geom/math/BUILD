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

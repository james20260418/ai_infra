load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# ============================================================================
# 通用工具库 — 纯头文件，零依赖
# ============================================================================
cc_library(
    name = "utils",
    hdrs = ["utils.h"],
    visibility = ["//visibility:public"],
)

# 通用工具单测（当前含 EditTimestampSuffix：模型编辑器保存文件名的时间戳后缀）。
cc_test(
    name = "utils_test",
    srcs = ["utils_test.cc"],
    deps = [
        ":utils",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

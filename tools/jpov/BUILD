load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# JPOV 主库
# ============================================================================
cc_library(
    name = "jpov",
    srcs = ["src/jpov.cc"],
    hdrs = ["include/jpov/jpov.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:input_snapshot",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "@glfw_static//:glfw",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkopts = ["-lGL"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# 用户示例：空窗口应用
# ============================================================================
cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
)

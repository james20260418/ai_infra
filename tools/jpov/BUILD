load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# JPOV Renderer 内部库（GL 复杂度消化器）
# ============================================================================
cc_library(
    name = "renderer",
    srcs = ["src/renderer.cc"],
    hdrs = ["src/renderer.h"],
    visibility = ["//visibility:private"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "@glfw_static//:glfw",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# ============================================================================
# JPOV 主库
# ============================================================================
cc_library(
    name = "jpov",
    srcs = ["src/jpov.cc"],
    hdrs = ["include/jpov/jpov.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":renderer",
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
# 用户示例：Lissajous polyline demo
# ============================================================================
cc_library(
    name = "jpov_demo_lib",
    srcs = ["demo/jpov_demo.cc"],
    hdrs = [],
    deps = [":jpov"],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_demo",
    deps = [":jpov_demo_lib"],
)

# ============================================================================
# Rect demo（窗口坐标 → FBO 映射验证）
# ============================================================================
cc_library(
    name = "jpov_demo_rect_lib",
    srcs = ["demo/jpov_demo_rect.cc"],
    hdrs = [],
    deps = [":jpov"],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_demo_rect",
    deps = [":jpov_demo_rect_lib"],
)

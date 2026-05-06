load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# Renderer — GL FBO + 动态分辨率渲染核心
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
        "@opencv_static//:opencv",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# ============================================================================
# JPOV 主框架库 — GLFW 窗口 + 输入 + Renderer
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
# 示例：Rect Demo — 演示窗口坐标与渲染分辨率裁剪
# ============================================================================
cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
)

# ============================================================================
# 示例：Screenshot Demo — 绘制后保存 PNG 截图
# ============================================================================
cc_binary(
    name = "jpov_screenshot_demo",
    srcs = ["demo/jpov_screenshot_demo.cc"],
    deps = [
        ":jpov",
        "//tools/common:utils",
    ],
)

# ============================================================================
# 示例：Polyline Demo — 绘制折线后保存 PNG 截图
# ============================================================================
cc_binary(
    name = "jpov_polyline_demo",
    srcs = ["demo/jpov_polyline_demo.cc"],
    deps = [
        ":jpov",
        "//tools/common:utils",
    ],
)

cc_binary(
    name = "jpov_polyline_verify",
    srcs = ["demo/jpov_polyline_verify.cc"],
    deps = [
        ":jpov",
        "//tools/common:utils",
    ],
)

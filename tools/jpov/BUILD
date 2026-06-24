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
        "//geom/common:common",
        "@glog_static//:glog",
        "//third_party/stb:stb_image_write",
    ] + select({
        "@platforms//os:windows": [
            "@glfw_mingw//:glfw",
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": ["@glfw_static//:glfw"],
    }),
    copts = ["-std=c++20", "-DSTB_IMAGE_WRITE_IMPLEMENTATION"],
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
    ] + select({
        "@platforms//os:windows": ["@glfw_mingw//:glfw"],
        "//conditions:default": ["@glfw_static//:glfw"],
    }),
    copts = ["-std=c++20"],
    linkopts = select({
        "@platforms//os:windows": ["-lopengl32"],
        "//conditions:default": ["-lGL"],
    }),
    linkstatic = 1,
)

# ============================================================================
# 示例：Rect Demo — 演示窗口坐标与渲染分辨率裁剪
# ============================================================================
cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_demo.exe",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
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
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# 示例：Polyline Demo — 绘制后保存 PNG 截图
# ============================================================================
cc_binary(
    name = "jpov_polyline_demo",
    srcs = ["demo/jpov_polyline_demo.cc"],
    deps = [
        ":jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_polyline_verify",
    srcs = ["demo/jpov_polyline_verify.cc"],
    deps = [
        ":jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

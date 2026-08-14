load("@rules_cc//cc:defs.bzl", "cc_library")

# JPOV 主框架库 — GLFW 窗口 + 输入 + Renderer
cc_library(
    name = "jpov",
    srcs = ["jpov.cc"],
    hdrs = ["jpov.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/src:renderer",
        "//tools/jpov/interface:input_snapshot",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/interface:mesh",
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

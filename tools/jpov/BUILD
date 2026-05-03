load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

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

cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
)

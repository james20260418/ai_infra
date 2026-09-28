load("@rules_cc//cc:defs.bzl", "cc_library")

# BurningRenderer — 「燃烧」特效（物体表面着火：火舌 + 烟）。
# 与 effect/fire_render（火焰＝点上的自足发光体）区分：
#   燃烧由**物体几何**驱动，火源沿表面采样、跟着物体走。
# 全 static 工具集；shader 源码作为常量暴露，由 Renderer 经 ShaderManager 编译。
cc_library(
    name = "burning_renderer",
    srcs = ["burning_renderer.cc"],
    hdrs = ["burning_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:burning_command",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:mesh",
        "//geom/common:vec",
        "@glog_static//:glog",
    ] + select({
        "@platforms//os:windows": [
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": [],
    }),
    copts = ["-std=c++20"],
    linkstatic = 1,
)

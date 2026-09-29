load("@rules_cc//cc:defs.bzl", "cc_library")

# VolumetricRenderer — 体积特效渲染（预实验：真 3D 体积团，逐像素 ray-march）。
# 并列于 fire / burning / particle 的一条特效线（全 static）。
# 它自己不含 shader program（源码作为常量暴露，由 Renderer 经 ShaderManager 编译）。
cc_library(
    name = "volumetric_renderer",
    srcs = ["volumetric_renderer.cc"],
    hdrs = ["volumetric_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:camera",
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

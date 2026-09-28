load("@rules_cc//cc:defs.bzl", "cc_library")

# FireRenderer — 程序化火焰特效（3D billboard 面片 + 加法混合，无纹理）。
# 并列于 primitives3d_renderer 的一个「特效 pass」工具集（全 static）。
# 它自己不含 shader program（源码作为常量暴露，由 Renderer 经 ShaderManager 编译）。
cc_library(
    name = "fire_renderer",
    srcs = ["fire_renderer.cc"],
    hdrs = ["fire_renderer.h"],
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

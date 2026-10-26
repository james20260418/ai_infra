load("@rules_cc//cc:defs.bzl", "cc_library")

# SsrRenderer — 屏幕空间反射（独立 pass：不透明 3D 之后、半透明/雾之前）。
#
# 自持全部 GL 资源（属性缓冲 / Hi-Z 金字塔 / 反射缓冲），不修改任何既有 renderer 的
# shader：反射体的表面属性由本模块自己的 attr pass 画出（只画 ssr_reflect 标记的物体）。
# 设计：docs/jpov_ssr_design.md
cc_library(
    name = "ssr_renderer",
    srcs = ["ssr_renderer.cc"],
    hdrs = [
        "ssr_renderer.h",
        "ssr_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:render_command",
        "//tools/jpov/src:mesh_manager",
        "//tools/jpov/src:object3d_renderer",
        "//tools/jpov/src:primitives3d_renderer",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:texture_manager",
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

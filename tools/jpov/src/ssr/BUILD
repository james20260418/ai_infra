load("@rules_cc//cc:defs.bzl", "cc_library")

# SSR 算法数学（GL-free，header-only）。
# 单测的落点是它，着色器的算式也来自它 —— 两侧只允许有这一份"文字形式"。
cc_library(
    name = "ssr_math",
    hdrs = ["ssr_math.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SsrRenderer — 屏幕空间反射 v2（体素桩 + 分段并行）。
# 设计见 docs/jpov_ssr_v2_design.md。
cc_library(
    name = "ssr_renderer",
    srcs = ["ssr_renderer.cc"],
    hdrs = [
        "ssr_renderer.h",
        "ssr_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_math",
        "//tools/jpov/src:shader_manager",
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

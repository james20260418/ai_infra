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

# SSR 的 GLSL（common / march / resolve / 属性 pass）。单独成 target：
# 三个使用方（模块 / 降采样 / 融合）都要 include 它，但都不该因此链接整份实现。
cc_library(
    name = "ssr_shader",
    hdrs = ["ssr_shader.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SsrRenderer — 屏幕空间反射 v2（体素桩 + 分段并行）。
# 设计见 docs/jpov_ssr_v2_design.md。
cc_library(
    name = "ssr_renderer",
    srcs = ["ssr_renderer.cc"],
    hdrs = ["ssr_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_math",
        ":ssr_shader",
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

# SSR 输入准备：全分辨率 (color,depth,attr) → SSR 分辨率 (color,depth,normal)。
cc_library(
    name = "ssr_downsample",
    srcs = ["ssr_downsample.cc"],
    hdrs = ["ssr_downsample.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_shader",
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

# SSR 融合：升采样 + 用 SSR 结果替换环境镜面项。
cc_library(
    name = "ssr_fusion",
    srcs = ["ssr_fusion.cc"],
    hdrs = ["ssr_fusion.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_shader",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/src:env_specular",
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

# SSR 接入层（编排）：HDR 副本 → 降采样 → 模块 → 融合；属性 pass 的 program 也在这里。
cc_library(
    name = "ssr_integration",
    srcs = ["ssr_integration.cc"],
    hdrs = ["ssr_integration.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_downsample",
        ":ssr_fusion",
        ":ssr_renderer",
        ":ssr_shader",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/src:shader_manager",
        # 蒙皮属性 pass 复用主 pass 的 kSkinnedVs（不抄蒙皮代码）
        "//tools/jpov/src/skeleton:skeleton",
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

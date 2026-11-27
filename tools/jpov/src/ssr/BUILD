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

# SSR 粗糙度预滤波：A 预乘 mip 链（覆盖率加权 + 空像素填充）。
# 它决定了"磨砂"到底有多糊：合成层按逐像素 roughness 采 textureLod。
cc_library(
    name = "ssr_filter",
    srcs = ["ssr_filter.cc"],
    hdrs = ["ssr_filter.h"],
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

# 同面并集柱体：SSR 分辨率的"每格 back"图（命中判据的厚度源之一）。
cc_library(
    name = "ssr_slab_back",
    srcs = ["ssr_slab_back.cc"],
    hdrs = ["ssr_slab_back.h"],
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

# 升采样：SSR 分辨率的 A 预乘 mip 链 → 全分辨率反射图（+ 背面 flag）。
cc_library(
    name = "ssr_upsample",
    srcs = ["ssr_upsample.cc"],
    hdrs = ["ssr_upsample.h"],
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

# SSR 装置（util）：Source + Query → 全分辨率反射图。
# 位置无关：只有这一个 target 是"任何消费者都可以直接用"的入口，
# 它不认识 SsrConfig / 环境探针 / HDR 目标（那些属于消费者）。
cc_library(
    name = "ssr_chain",
    srcs = ["ssr_chain.cc"],
    hdrs = ["ssr_chain.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_downsample",
        ":ssr_filter",
        ":ssr_renderer",
        ":ssr_slab_back",
        ":ssr_upsample",
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

# 不透明场景的消费端：用 SSR 结果替换环境镜面项（"实心→实心"的位置知识）。
cc_library(
    name = "ssr_composite",
    srcs = ["ssr_composite.cc"],
    hdrs = ["ssr_composite.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_shader",
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

# 不透明场景的接入层（胶水）：属性 pass 的 program + 装置 + 合成。
cc_library(
    name = "ssr_opaque_reflection",
    srcs = ["ssr_opaque_reflection.cc"],
    hdrs = ["ssr_opaque_reflection.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ssr_chain",
        ":ssr_composite",
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

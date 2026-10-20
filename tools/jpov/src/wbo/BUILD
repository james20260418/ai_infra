load("@rules_cc//cc:defs.bzl", "cc_library")

# WBO（加权混合 OIT）目标的唯一实现：FBO / 附件 / clear / draw 状态 / 还原。
# 设计见 tools/jpov/docs/jpov_strip3d_translucent_design.md §5 与 wbo_target.h 头注释。
cc_library(
    name = "wbo_target",
    srcs = ["wbo_target.cc"],
    hdrs = ["wbo_target.h"],
    visibility = ["//visibility:public"],
    deps = [
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

# WBO 共享 GLSL：权重函数（唯一权威副本）+ 全屏解析（覆盖集/加法集）。
cc_library(
    name = "wbo_shader",
    hdrs = ["wbo_shader.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

load("@rules_cc//cc:defs.bzl", "cc_library")

# SmaaRenderer — 屏幕空间抗锯齿 SMAA 1x（后处理 pass，与 bloom / tone map 平级）。
# 自包含：三趟 shader + 参考实现的预计算 LUT（area/search）均在本目录。
cc_library(
    name = "smaa_renderer",
    srcs = ["smaa_renderer.cc"],
    hdrs = [
        "smaa_renderer.h",
        "smaa_shader.h",
        # SMAA 预计算查找纹理（参考实现 MIT 数据，见各自文件头）。
        "smaa_area_tex.h",
        "smaa_search_tex.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":smaa_shader",
        ":smaa_lookup_tex",
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

# 三趟 SMAA shader（GLSL 330 core 源串，header-only）。
cc_library(
    name = "smaa_shader",
    hdrs = ["smaa_shader.h"],
    visibility = ["//visibility:public"],
)

# SMAA 预计算 LUT 数据（header-only），与 shader 同源版本。
cc_library(
    name = "smaa_lookup_tex",
    hdrs = [
        "smaa_area_tex.h",
        "smaa_search_tex.h",
    ],
    visibility = ["//visibility:public"],
)

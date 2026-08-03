load("@rules_cc//cc:defs.bzl", "cc_library")

# TextureManager — GPU 纹理生命周期管理（PNG 加载/去重/释放）
cc_library(
    name = "texture_manager",
    srcs = ["texture_manager.cc"],
    hdrs = ["texture_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//third_party/stb:stb_image",
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

# FontManager — 字体加载 / 光栅化 / 图集管理 / 顶点生成
cc_library(
    name = "font_manager",
    srcs = ["font_manager.cc"],
    hdrs = ["font_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:render_command",
        "@glog_static//:glog",
        "//third_party/stb:stb_truetype",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# Renderer — GL FBO + 动态分辨率渲染核心
cc_library(
    name = "renderer",
    srcs = ["renderer.cc"],
    hdrs = ["renderer.h"],
    data = ["//tools/jpov/fonts:all_fonts"],
    visibility = ["//visibility:public"],
    deps = [
        ":font_manager",
        ":texture_manager",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "//geom/common:common",
        "@glog_static//:glog",
        "//third_party/stb:stb_image_write",
    ] + select({
        "@platforms//os:windows": [
            "@glfw_mingw//:glfw",
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": ["@glfw_static//:glfw"],
    }),
    copts = ["-std=c++20", "-DSTB_IMAGE_WRITE_IMPLEMENTATION"],
    linkstatic = 1,
)

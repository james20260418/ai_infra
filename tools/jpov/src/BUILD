load("@rules_cc//cc:defs.bzl", "cc_library")

# Renderer — GL FBO + 动态分辨率渲染核心
cc_library(
    name = "renderer",
    srcs = ["renderer.cc"],
    hdrs = ["renderer.h"],
    data = [
        "//tools/jpov:fonts/NotoSansCJK-Regular.ttc",
        "//tools/jpov:fonts/DejaVuSans.ttf",
    ],
    # data 使用完整 target 引用（根 BUILD 声明 exports_files），见 tools/jpov/BUILD
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "//geom/common:common",
        "@glog_static//:glog",
        "//third_party/stb:stb_image_write",
        "//third_party/stb:stb_truetype",
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

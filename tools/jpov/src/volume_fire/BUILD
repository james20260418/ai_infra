load("@rules_cc//cc:defs.bzl", "cc_library")

# 体积火（逐像素 raymarch）—— 渲染器。
# 把一团 .jpovvol 沿视线积分，作为薄层写进 WBO 透明纹理。
# 设计见 tools/jpov/docs/jpov_volumetric_fire_design.md。
cc_library(
    name = "volume_fire_renderer",
    srcs = ["volume_fire_renderer.cc"],
    hdrs = ["volume_fire_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:volume_fire_types",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:volume_manager",
        "@glog_static//:glog",
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
    ] + select({
        "@platforms//os:windows": [
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": [],
    }),
    copts = ["-std=c++20"],
    linkstatic = 1,
)

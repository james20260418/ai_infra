load("@rules_cc//cc:defs.bzl", "cc_library")

# 全局水面（水/海）：极坐标 LOD 网格 + 天光反射 + 水下吸收/散射 + 水下整屏介质。
# 接口层类型见 //tools/jpov/interface:water_types；pass 位置见 src/renderer.cc。
cc_library(
    name = "water_renderer",
    srcs = ["water_renderer.cc"],
    hdrs = [
        "water_renderer.h",
        "water_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:water_types",
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

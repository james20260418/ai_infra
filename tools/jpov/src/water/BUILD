load("@rules_cc//cc:defs.bzl", "cc_library")

# 水皮的 CPU 参考模型（GL-free）：视线∩平面、菲涅尔（含全内反射）、
# 反射方向夹地平线、深度拒绝。单测与 GPU 对拍共用这一份（见头文件顶部）。
cc_library(
    name = "water_skin_math",
    hdrs = ["water_skin_math.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水皮渲染器：全屏三角形 + 解析求交 → WBO 覆盖集（accum/reveal/depth）→ over 合成。
cc_library(
    name = "water_skin_renderer",
    srcs = ["water_skin_renderer.cc"],
    hdrs = [
        "water_skin_renderer.h",
        "water_skin_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":water_skin_math",
        "//tools/jpov/interface:camera",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/wbo:wbo_shader",
        "//tools/jpov/src/wbo:wbo_target",
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

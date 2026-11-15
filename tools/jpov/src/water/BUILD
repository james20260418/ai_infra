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

# 水体介质的 CPU 参考模型（GL-free）：半空间 + 均质 ⇒ 闭式 τ/S（含 Snell 折射、
# 菲涅尔界面透射、单次散射 + 环境项）。单测与 GPU 对拍共用这一份；
# water_medium_shader.h 是同一套公式的 GLSL 版（"同源"，改一处必须改另一处）。
cc_library(
    name = "water_medium_math",
    hdrs = [
        "water_medium_math.h",
        "water_medium_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":water_skin_math",
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水下光照衰减（三个 PBR 主 pass 共用）：GLSL 片段 + uniform 上传。
# 只做“照明侧”（折射方向 / 太阳路径衰减 / 环境随深度衰减）；视线侧在合成趟。
cc_library(
    name = "water_underwater_shading",
    srcs = ["water_underwater_shading.cc"],
    hdrs = ["water_underwater_shading.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":water_medium_math",
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

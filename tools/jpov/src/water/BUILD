load("@rules_cc//cc:defs.bzl", "cc_library")

# 水体光学原语（GL-free，header-only）：菲涅尔（含全内反射）/ 临界角 / 折射方向 /
# HG 相位 / 指数段积分 / 界面透射率常量。**全工程唯一一份**物理公式出处。
# GLSL 侧的等价实现见 water_medium_shader.h 的 `Wm*`，由 GPU 对拍测试兜底。
cc_library(
    name = "water_optics",
    hdrs = ["water_optics.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水体介质的 CPU 参考模型（GL-free）：半空间 + 均质 ⇒ 闭式 τ/S（Snell 折射、
# 界面透射、单次散射 + 环境项）。单测与 GPU 对拍共用这一份；
# water_medium_shader.h 是同一套公式的 GLSL 版（"同源"，改一处必须改另一处）。
cc_library(
    name = "water_medium_math",
    hdrs = [
        "water_medium_math.h",
        "water_medium_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":water_optics",
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水皮的 CPU 参考（GL-free）：视线∩平面、菲涅尔（含全内反射）、反射方向夹地平线、
# 深度拒绝、层语义（C / α / 射线距离）。单测与 GPU 对拍共用这一份。
cc_library(
    name = "water_skin_math",
    hdrs = ["water_skin_math.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":water_optics",
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水皮渲染器：全屏三角形 + 解析求交 → **只产一层** WBO 覆盖集（accum/reveal/att3）。
# 合成由 fog_composite 按「场段 ↔ 层」交替完成，本类不做 composite。
cc_library(
    name = "water_skin_renderer",
    srcs = ["water_skin_renderer.cc"],
    hdrs = [
        "water_skin_renderer.h",
        "water_skin_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":water_medium_math",
        ":water_skin_math",
        "//tools/jpov/interface:camera",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:transparent_layer",
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

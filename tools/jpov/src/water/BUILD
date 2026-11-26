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

# 水面波浪（GL-free CPU 参考 + GLSL 同源）：纯高度场 Gerstner 谱 + flow map 调制，
# 提供 h(x,z,t) / 解析梯度 / 法线 / 视线求交（平面初值 + Newton）。
# 无波（enabled=false 或 num_waves=0）⇒ h≡0，调用方行为与纯平面逐字节一致。
cc_library(
    name = "water_wave_math",
    hdrs = [
        "water_wave_math.h",
        "water_wave_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 波浪谱 → GLSL uniform 的**唯一一份**上传实现（水皮 + 天球折射共用）。
# 为什么必须共用：两趟调同一个 WaveSurfaceHit，少传一个字段就会让 Snell 窗缘错位。
cc_library(
    name = "water_wave_uniforms",
    srcs = ["water_wave_uniforms.cc"],
    hdrs = ["water_wave_uniforms.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":water_wave_math",
        "//tools/jpov/interface:render_command",
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

# 水皮的 CPU 参考（GL-free）：视线∩平面、菲涅尔（含全内反射）、反射方向夾地平线、：视线∩平面、菲涅尔（含全内反射）、反射方向夹地平线、
# 深度拒绝、层语义（C / α / 射线距离）。单测与 GPU 对拍共用这一份。
cc_library(
    name = "water_skin_math",
    hdrs = ["water_skin_math.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":water_wave_math",
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
        ":water_wave_math",
        ":water_wave_uniforms",
        "//tools/jpov/interface:camera",
        "//tools/jpov/src:csm_shadow_glsl",   # CSM 单次采样（与 fog/strip 同一份）
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:transparent_layer",
        "//tools/jpov/src/lighting:ggx_specular",
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

# 水皮 V2（单像素多法线采样）的整份 pass：与 water_skin_renderer **互斥**，
# 由 WaterWaves::v2 二选一（见 docs/jpov_water_skin_v2_design.md）。
# 输入契约直接复用 V1 的 Inputs（面板滑条共用），输出 WBO 三槽语义与 V1 逐项相同。
cc_library(
    name = "water_skin_v2_renderer",
    srcs = ["water_skin_v2_renderer.cc"],
    hdrs = [
        "water_skin_v2_renderer.h",
        "water_skin_v2_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":water_medium_math",
        ":water_skin_math",
        ":water_skin_renderer",   # 共享输入契约（WaterSkinRenderer::Inputs）
        ":water_wave_math",
        ":water_wave_uniforms",
        "//tools/jpov/interface:camera",
        "//tools/jpov/src:csm_shadow_glsl",   # CSM 单次采样（与 fog/strip 同一份）
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:transparent_layer",
        "//tools/jpov/src/lighting:ggx_specular",
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

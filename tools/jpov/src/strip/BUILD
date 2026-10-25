load("@rules_cc//cc:defs.bzl", "cc_library")

# 半透明条带：CPU 侧几何展开 + 输入校验（GL-free，可单测）。
cc_library(
    name = "strip_geometry",
    hdrs = ["strip_geometry.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:strip_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 半透明条带渲染器：整批一次 draw call 画进 1× WBO 覆盖集，再 over 合成进 HDR。
cc_library(
    name = "strip_renderer",
    srcs = ["strip_renderer.cc"],
    hdrs = ["strip_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":strip_geometry",
        "//geom/math:mat4",
        "//geom/math:piecewise_linear_function",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",   # DirectionalLight / ShadowConfig / CascadeFBO（CSM 输入）
        "//tools/jpov/interface:strip_types",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:texture_manager",
        "//tools/jpov/src:texture_units",
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

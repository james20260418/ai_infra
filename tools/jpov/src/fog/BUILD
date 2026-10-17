load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Fog（雾火）—— 统一体积雾/火子渲染器（froxel 屏幕空间摊销 Z 轴采样）。
#
# 与 object3d / horizon_fog 平级、单一自包含。设计见 tools/jpov/docs/jpov_froxel_design.md。
cc_library(
    name = "fog_renderer",
    srcs = ["fog_renderer.cc"],
    hdrs = [
        "fog_renderer.h",
        "fog_shader.h",
        "fog_lower.h",
        "fog_body.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":convex_hull_tiles",
        "//tools/jpov/src:matrix_util",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
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

# FogCompositeRenderer — 雾场 + 多层 WBO 透明纹理的唯一合成趟（全屏多段积分）。
#
# 按**趟**分层：FogRenderer 产 froxel 场，本类把它与 0..N 个透明层按代表深度交替积分。
cc_library(
    name = "fog_composite_renderer",
    srcs = ["fog_composite_renderer.cc"],
    hdrs = ["fog_composite_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":fog_renderer",
        ":fog_shader",
        "//tools/jpov/src:matrix_util",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:transparent_layer",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
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

# FogLower — GL-free CPU 工具（PointFog→FogBody 降维 + 屏幕 tile 剪枝表）。
cc_library(
    name = "fog_lower",
    hdrs = [
        "fog_lower.h",
        "fog_body.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":convex_hull_tiles",
        "//tools/jpov/interface:render_command",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# FogLower tests（降维 + tile 剪枝表，GL-free）。
cc_test(
    name = "fog_lower_test",
    srcs = ["fog_lower_test.cc"],
    deps = [
        ":fog_lower",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# ConvexHullTiles — fog tile culling 专用：box 屏幕凸包（≤8 点）→ 覆盖的 tile（扫描线）。
# context 无关、无 GL、可单测；点用 geom::Vec2d。
cc_library(
    name = "convex_hull_tiles",
    hdrs = ["convex_hull_tiles.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:check",
        "//geom/common:vec",
    ],
    copts = ["-std=c++20"],
)

# ConvexHullTiles tests（含随机 8 点 cross-validate）
cc_test(
    name = "convex_hull_tiles_test",
    srcs = ["convex_hull_tiles_test.cc"],
    deps = [
        ":convex_hull_tiles",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Fog shader 源码（header-only；只有 GLSL 字符串常量，无 GL 依赖）。
# 单拎成 lib 供契约单测读取 GLSL 里的布局标记。
cc_library(
    name = "fog_shader",
    hdrs = ["fog_shader.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
)

# 布局契约单测：解析 fog_shader.h 的 //@FOGLAYOUT 标记 / #define，
# 与 fog_body.h 的 kFogBodyLayout / fog_layout 常量逐项比对（防 CPU-GLSL 静默漂移）。
cc_test(
    name = "fog_contract_test",
    srcs = ["fog_contract_test.cc"],
    deps = [
        ":fog_lower",
        ":fog_shader",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

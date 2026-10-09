load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# FireFog（雾火）—— 统一体积雾/火子渲染器（froxel 屏幕空间摊销 Z 轴采样）。
#
# 与 object3d / horizon_fog 平级、单一自包含。设计见 tools/jpov/docs/jpov_froxel_design.md。
cc_library(
    name = "fire_fog_renderer",
    srcs = ["fire_fog_renderer.cc"],
    hdrs = [
        "fire_fog_renderer.h",
        "fire_fog_shader.h",
        "fire_fog_lower.h",
        "fog_body.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":convex_hull_tiles",
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

# FireFogLower — GL-free CPU 工具（PointFog→FogBody 降维 + 屏幕 tile 剪枝表）。
cc_library(
    name = "fire_fog_lower",
    hdrs = [
        "fire_fog_lower.h",
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

# FireFogLower tests（降维 + tile 剪枝表，GL-free）。
cc_test(
    name = "fire_fog_lower_test",
    srcs = ["fire_fog_lower_test.cc"],
    deps = [
        ":fire_fog_lower",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# ConvexHullTiles — fire_fog tile culling 专用：box 屏幕凸包（≤8 点）→ 覆盖的 tile（扫描线）。
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

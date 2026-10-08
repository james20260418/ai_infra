load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# FireFog（雾火）—— 统一体积雾/火子渲染器（屏幕 tile 剪枝 + 逐像素 ZDist）。
#
# 与 object3d / horizon_fog 平级、单一自包含。设计见 tools/jpov/docs/jpov_fire_fog_design.md。
# 本 PR 只落头文件与框架：数据模型（fog_body.h）+ 渲染器签名（fire_fog_renderer.h）；
# pass 实现见设计文档 §10.6 的 M1a。
cc_library(
    name = "fire_fog_renderer",
    srcs = ["fire_fog_renderer.cc"],
    hdrs = [
        "fire_fog_renderer.h",
        "fog_body.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
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

# ZDistFunction — ZDist（逐像素深度函数）的 CPU 参考实现（GL-free）：
#   z 段加法累加（乱序可加）+ 降采样到 ≤8 控制点（端点保留 ⇒ 总上升量守恒）+ 末端积分
#   + GPU 纹理布局打包。供 shader 对照 / debug / gold。设计见 docs/jpov_fire_fog_design.md §16。
cc_library(
    name = "zdist_function",
    hdrs = [
        "zdist_function.h",
        "zdist_texture_layout.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/math:size_limited_piecewise_linear_function",
        "//tools/jpov/interface:render_command",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# ZDistFunction tests
cc_test(
    name = "zdist_function_test",
    srcs = ["zdist_function_test.cc"],
    deps = [
        ":zdist_function",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

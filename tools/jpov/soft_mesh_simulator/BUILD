load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# ============================================================================
# 软体仿真器 — 独立包
#
# 边界：**纯 CPU、零 GL、可单测**。本包不依赖任何渲染层，也不依赖 tools/jpov/demo。
# 显示（JPOV 渲染管线）与物理（本包）各自独立演进，互不污染。
#
# 对外只暴露一个概念：jpov::soft_mesh_simulator::Simulator
#   主入口 = Step(mesh, dt) → 新的 mesh（网格经 dt 秒动力学后的结果）
#
# 设计定稿见 DESIGN.md（签字版）—— 实现必须遵守它。
# ============================================================================

filegroup(
    name = "docs",
    srcs = ["DESIGN.md"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "soft_mesh_simulator",
    srcs = ["soft_mesh_simulator.cc"],
    hdrs = [
        "screen_projection.h",
        "soft_mesh_simulator.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:quaternion",
        "//geom/common:vec",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# point_in_mesh — 「点是否在网格体内」查询（纯 CPU，射线奇偶法）。
#
# 与本包的仿真器本体**互不依赖**：只用到 MeshData 与 geom 向量，故单列一个库，
# 便于将来的八叉树等结构直接复用，而不必牵扯整套物理。
cc_library(
    name = "point_in_mesh",
    srcs = ["point_in_mesh.cc"],
    hdrs = ["point_in_mesh.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

cc_test(
    name = "point_in_mesh_test",
    srcs = ["point_in_mesh_test.cc"],
    deps = [
        ":point_in_mesh",
        "//geom/common:vec",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "soft_mesh_simulator_test",
    srcs = ["soft_mesh_simulator_test.cc"],
    deps = [
        ":soft_mesh_simulator",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "screen_projection_test",
    srcs = ["screen_projection_test.cc"],
    deps = [
        ":soft_mesh_simulator",
        "//geom/common:vec",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

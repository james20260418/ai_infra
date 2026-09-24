load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# ============================================================================
# 软体仿真器 — 独立包
#
# 边界：**纯 CPU、零 GL、可单测**。本包不依赖任何渲染层，也不依赖 tools/jpov/demo。
# 显示（JPOV 渲染管线）与物理（本包）各自独立演进，互不污染。
#
# 对外只暴露一个概念：jpov::soft_mesh_simulator::Simulator
#   主入口 = Step(mesh, dt) → 新的 mesh（网格经 dt 秒动力学后的结果）
# ============================================================================

cc_library(
    name = "soft_mesh_simulator",
    srcs = ["soft_mesh_simulator.cc"],
    hdrs = ["soft_mesh_simulator.h"],
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

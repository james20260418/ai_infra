load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# 穿衣工具（clothing tool）— 独立包
#
# 需求（2026-09-27 Danis）：仿 soft_mesh_simulator 的查看器，做一个把「人体
# reference + 衣服模型」加载并显示出来的可视化工具；要求**单独文件夹和命名空间**
# （即 tools/jpov/clothing/ + namespace jpov::clothing，与 soft_mesh_simulator 互不牵连）。
#
# 本阶段：**加载 + 显示 + 粗调位置**（后台建最近邻三角形 + 左上角 x/y/z 填值面板），
# 仍不做对齐/穿衣物理。边界见 clothing_tool_app.h。
#
#   clothing_tool_app : 渲染核心 App（场景 + 面板宿主 + 唯一 OneIteration）
#   jpov_clothing_tool: 主程序（CLI 装配 + 交互/headless 分发）
# ============================================================================

# 坐标填值输入解析（纯函数；header-only，无 GL/glog 依赖），单列一个库便于纯单测。
cc_library(
    name = "clothing_axis_input",
    hdrs = ["clothing_axis_input.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
)

cc_library(
    name = "clothing_tool_app",
    srcs = ["clothing_init.cc"],
    hdrs = [
        "clothing_init.h",
        "clothing_tool_app.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":clothing_axis_input",
        "//geom/3d:triangle_matcher_3d",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:ui",
        "//tools/jpov/src:gltf_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 坐标填值输入解析（纯函数单测；无 GL）。
cc_test(
    name = "clothing_axis_input_test",
    srcs = ["clothing_axis_input_test.cc"],
    deps = [
        ":clothing_axis_input",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_clothing_tool",
    srcs = ["jpov_clothing_tool.cc"],
    deps = [
        ":clothing_tool_app",
        "//tools/jpov/include/jpov:jpov",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

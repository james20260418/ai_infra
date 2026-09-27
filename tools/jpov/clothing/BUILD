load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# 穿衣工具（clothing tool）— 独立包
#
# 需求（2026-09-27 Danis）：仿 soft_mesh_simulator 的查看器，做一个把「人体
# reference + 衣服模型」加载并显示出来的可视化工具；要求**单独文件夹和命名空间**
# （即 tools/jpov/clothing/ + namespace jpov::clothing，与 soft_mesh_simulator 互不牵连）。
#
# 本阶段（M0）只做「加载 + 显示」：不做对齐、不做穿衣物理。边界见 clothing_tool_app.h。
#
#   clothing_tool_app : 渲染核心 App（场景 + 面板宿主 + 唯一 OneIteration）
#   jpov_clothing_tool: 主程序（CLI 装配 + 交互/headless 分发）
# ============================================================================

cc_library(
    name = "clothing_tool_app",
    hdrs = ["clothing_tool_app.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:ui",
    ],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_clothing_tool",
    srcs = ["jpov_clothing_tool.cc"],
    deps = [
        ":clothing_tool_app",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

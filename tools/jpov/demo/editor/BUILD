load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# ============================================================================
# JPOV 模型编辑器（独立小工具）— retarget 前置组件
#
# 人手微调 glTF 的缩放/平移/旋转，与目标骨架配准。
# 场景与查看器同款（sunny day 光照 + 可调地面 + 右键环绕相机，复用
# //tools/jpov:view_config），面板换成放置控件，另消费左键横向 drag 做旋转。
#
# 目录自洽：本工具的全部代码/测试都在 demo/editor/ 下，不散落在上层。
#   model_placement : 放置数学（纯函数、header-only、与渲染后端零耦合）
#   editor_app      : 渲染核心 App（场景 + 放置面板 + 左键旋转消费）
#   jpov_model_editor : 主程序（交互事件循环）
#
# 构建：./tools/jpov/build_jpov_model_editor.sh
# 详见 tools/jpov/docs/jpov_model_editor_design.md
# ============================================================================

cc_library(
    name = "model_placement",
    hdrs = ["model_placement.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "editor_app",
    hdrs = ["editor_app.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":model_placement",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:ui",
    ],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_model_editor",
    srcs = ["jpov_model_editor.cc"],
    deps = [
        ":editor_app",
        ":model_placement",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 放置数学纯函数单测（世界轴任意顺序累积 / 正交归一 / 符号 / clamp）。
cc_test(
    name = "model_placement_test",
    srcs = ["model_placement_test.cc"],
    deps = [
        ":model_placement",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 左键 drag 归属回归（白盒 friend EditorApp）：锁死「拖滑条误触旋转」
# （实测发生过的 bug）与「面板几何单一真相」结构不变量。
cc_test(
    name = "editor_drag_ownership_test",
    srcs = ["editor_drag_ownership_test.cc"],
    deps = [
        ":editor_app",
        "//tools/jpov/include/jpov:jpov",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

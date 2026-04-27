load("@rules_cc//cc:defs.bzl", "cc_library")

# InputSnapshot — 帧级输入抽象
cc_library(
    name = "input_snapshot",
    hdrs = ["input_snapshot.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@glog_static//:glog",
    ],
)

# RenderCommand — 帧级渲染指令输出
cc_library(
    name = "render_command",
    hdrs = ["render_command.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
)

# Camera — 透视相机配置
cc_library(
    name = "camera",
    hdrs = ["camera.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":render_command",
    ],
)

# WindowInfo — 帧级窗口信息
cc_library(
    name = "window_info",
    hdrs = ["window_info.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@glog_static//:glog",
    ],
)

# jpov_interface — 聚合所有接口（方便用户一次性引用）
cc_library(
    name = "jpov_interface",
    hdrs = [
        "camera.h",
        "input_snapshot.h",
        "render_command.h",
        "window_info.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":camera",
        ":input_snapshot",
        ":render_command",
        ":window_info",
    ],
)

# sp_ui — smart plate UI（SDF 板材支撑的即时模式控件）
#
# 与 interface:ui（旧版，纯色 + CPU 三角化底框）**隔离**：本库是从 ui.cc 复制而来
# 的独立实现，只把控件底框换成 Plate2DCommand（见 src/plate2d）。
# Danis 2026-10-27：不求复用，但求隔离 —— 两边可以逐控件并排对照，改一边不会
# 误伤另一边；验收通过后再决定是否取代旧版。

cc_library(
    name = "sp_ui",
    srcs = ["sp_ui.cc"],
    hdrs = ["sp_ui.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:input_snapshot",
        "//tools/jpov/interface:pbr_material",
        "//tools/jpov/interface:render_command",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

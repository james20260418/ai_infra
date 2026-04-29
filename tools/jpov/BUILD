load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# ============================================================================
# JPOV 主库
# ============================================================================
cc_library(
    name = "jpov",
    srcs = ["src/jpov.cc"],
    hdrs = ["include/jpov/jpov.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:input_snapshot",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "@glog_static//:glog",
    ],
)

# ============================================================================
# 用户示例：空窗口应用
# ============================================================================
cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = [":jpov"],
)

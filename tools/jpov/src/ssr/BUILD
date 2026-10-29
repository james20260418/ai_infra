load("@rules_cc//cc:defs.bzl", "cc_library")

# SsrRenderer — 屏幕空间反射 v2（体素桩模型 + 区间重叠判据）。
# 设计见 docs/jpov_ssr_v2_design.md；本目录当前只有接口定义（实现另开 commit）。
cc_library(
    name = "ssr_renderer",
    srcs = ["ssr_renderer.cc"],
    hdrs = ["ssr_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/src:shader_manager",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

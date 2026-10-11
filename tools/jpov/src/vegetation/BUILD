load("@rules_cc//cc:defs.bzl", "cc_library")

# L-System 引擎 + 龟图解释器 —— 植被程序化生成的「拓扑骨架」第一步。
# GL-free、确定性，可单测。设计见 tools/jpov/docs/jpov_vegetation_design.md。
cc_library(
    name = "lsystem",
    srcs = ["lsystem.cc"],
    hdrs = ["lsystem.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 枝干管网格 —— 把骨架枝链扫成管状 mesh（GL-free、确定性、可单测）。
cc_library(
    name = "branch_mesh",
    srcs = ["branch_mesh.cc"],
    hdrs = ["branch_mesh.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":lsystem",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

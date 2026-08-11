load("@rules_cc//cc:defs.bzl", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

# ============================================================================
# OBJ 加载器单元测试 — 纯 CPU 解析（不渲染）
# ============================================================================
cc_test(
    name = "jpov_obj_loader_test",
    srcs = ["jpov_obj_loader_test.cc"],
    data = [
        ":beetle.obj",
        ":cube_hand.obj",
        ":suzanne.obj",
    ],
    deps = [
        "//tools/jpov/interface:mesh",
        "//tools/jpov/src:obj_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

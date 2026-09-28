package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Triangle3 — 空间三角形（3D，非退化；header-only，模板）
cc_library(
    name = "triangle_3",
    hdrs = ["triangle_3.h"],
    deps = [
        "//geom/common:check",
        "//geom/common:common",
        "//geom/common:vec",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "triangle_3_test",
    srcs = ["triangle_3_test.cc"],
    deps = [
        ":triangle_3",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# TriangleMatcher3d — 「点到最近三角形」查询容器（体素栅格加速；header-only，模板）
cc_library(
    name = "triangle_matcher_3d",
    hdrs = ["triangle_matcher_3d.h"],
    deps = [
        ":triangle_3",
        "//geom/common:check",
        "//geom/common:grid",
        "//geom/common:grid_map",
        "//geom/common:vec",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "triangle_matcher_3d_test",
    srcs = ["triangle_matcher_3d_test.cc"],
    deps = [
        ":triangle_3",
        ":triangle_matcher_3d",
        "//geom/common:common",
        "//geom/common:vec",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

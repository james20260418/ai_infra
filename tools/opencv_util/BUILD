package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "opencv_util",
    srcs = ["opencv_util.cpp"],
    hdrs = ["opencv_util.h"],
    deps = [
        "//geom/common:vec",
        "@glog_static//:glog",
        "@opencv_static//:opencv",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

cc_test(
    name = "opencv_util_test",
    srcs = ["opencv_util_test.cpp"],
    deps = [
        ":opencv_util",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

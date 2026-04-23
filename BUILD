package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test", "cc_binary", "cc_proto_library")
load("@rules_proto//proto:defs.bzl", "proto_library")

cc_library(
    name = "a_plus_b",
    srcs = ["a_plus_b.cc"],
    hdrs = ["a_plus_b.h"],
    copts = ["-std=c++20"],
)

cc_test(
    name = "a_plus_b_test",
    srcs = ["a_plus_b_test.cc"],
    deps = [
        ":a_plus_b",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

proto_library(
    name = "math_proto",
    srcs = ["math.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "math_cc_proto",
    deps = [":math_proto"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "math_proto_test",
    srcs = ["math_proto_test.cc"],
    deps = [
        ":math_cc_proto",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

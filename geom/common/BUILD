package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")

# Documentation
filegroup(
    name = "docs",
    srcs = ["README.md"],
    visibility = ["//visibility:public"],
)

# Basic utility libraries
cc_library(
    name = "check",
    hdrs = ["check.h"],
    deps = [
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "math_util",
    hdrs = ["math_util.h"],
    copts = ["-std=c++20"],
)

cc_library(
    name = "common",
    hdrs = ["common.h"],
    srcs = ["common.cpp"],
    deps = [
        ":check",
    ],
    copts = ["-std=c++20"],
)

# Proto definitions
proto_library(
    name = "vec_proto",
    srcs = ["proto/vec.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "vec_cc_proto",
    deps = [":vec_proto"],
    visibility = ["//visibility:public"],
)

# Vec library
cc_library(
    name = "vec",
    hdrs = ["vec.h"],
    deps = [
        ":check",
        ":common",
        ":math_util",
        ":vec_cc_proto",
    ],
    copts = ["-std=c++20"],
)

# Vec tests (GTest format)
cc_test(
    name = "vec_test",
    srcs = ["vec_test.cc"],
    deps = [
        ":vec",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)
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

# Quaternion / 旋转工具（骨架 pose 用）。详见 quaternion.h 头部设计取舍。
cc_library(
    name = "quaternion",
    hdrs = ["quaternion.h"],
    deps = [
        ":vec",
    ],
    copts = ["-std=c++20"],
)

# Quaternion tests
cc_test(
    name = "quaternion_test",
    srcs = ["quaternion_test.cc"],
    deps = [
        ":quaternion",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Grid — N 维等距栅格的坐标↔索引换算（header-only，模板）
cc_library(
    name = "grid",
    hdrs = ["grid.h"],
    deps = [
        ":check",
    ],
    copts = ["-std=c++20"],
)

# GridMap — 以格索引为键的稀疏栅格（哈希表，header-only，模板）
cc_library(
    name = "grid_map",
    hdrs = ["grid_map.h"],
    deps = [
        ":grid",
    ],
    copts = ["-std=c++20"],
)

# Grid tests
cc_test(
    name = "grid_test",
    srcs = ["grid_test.cc"],
    deps = [
        ":grid",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# GridMap tests（对稠密参考实现做一致性 fuzz）
cc_test(
    name = "grid_map_test",
    srcs = ["grid_map_test.cc"],
    deps = [
        ":common",
        ":grid_map",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)
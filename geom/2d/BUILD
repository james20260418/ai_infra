package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Segment2 library
cc_library(
    name = "segment_2",
    hdrs = ["segment_2.h"],
    deps = [
        "//geom/common:vec",
        "//geom/common:check",
        "//geom/common:common",
        "//geom/common:math_util",
    ],
    copts = ["-std=c++20"],
)

# Polyline2d library
cc_library(
    name = "polyline_2d",
    srcs = ["polyline_2d.cpp"],
    hdrs = ["polyline_2d.h"],
    deps = [
        ":segment_2",
        "//geom/common:vec",
        "//geom/common:check",
        "//geom/common:common",
        "//geom/common:math_util",
    ],
    copts = ["-std=c++20"],
)

# AABox2 library (moved from geometry_legacy)
cc_library(
    name = "aabox2",
    hdrs = ["aabox2.h"],
    deps = [
        "//geom/common:check",
    ],
    copts = ["-std=c++20"],
)


# Segment2 tests
cc_test(
    name = "segment_2_test",
    srcs = ["segment_2_test.cc"],
    deps = [
        ":segment_2",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Polyline2d tests
cc_test(
    name = "polyline_2d_test",
    srcs = ["polyline_2d_test.cc"],
    deps = [
        ":polyline_2d",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# AABox2 tests
cc_test(
    name = "aabox2_test",
    srcs = ["aabox2_test.cc"],
    deps = [
        ":aabox2",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)
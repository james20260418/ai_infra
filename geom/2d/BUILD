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

# Geometry legacy (AABox2)
cc_library(
    name = "geometry_legacy",
    hdrs = ["geometry_legacy.h"],
    deps = [
        "//geom/common:vec",
        "//geom/common:check",
    ],
    copts = ["-std=c++20"],
)

# Tests will be added later
# cc_test(
#     name = "segment_2_test",
#     srcs = ["segment_2_test.cc"],
#     deps = [
#         ":segment_2",
#         "@gtest//:gtest_main",
#     ],
#     copts = ["-std=c++20"],
# )
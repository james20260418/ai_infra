package(default_visibility = ["//visibility:public"])

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
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

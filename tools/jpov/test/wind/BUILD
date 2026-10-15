load("@rules_cc//cc:defs.bzl", "cc_test")

# 植被风顶点标注派生 —— 单元测试（GL-free、纯 CPU）。
cc_test(
    name = "wind_flex_test",
    srcs = ["wind_flex_test.cc"],
    deps = [
        "//tools/jpov/src:wind_flex",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

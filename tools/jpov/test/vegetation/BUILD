load("@rules_cc//cc:defs.bzl", "cc_test")

# L-System 引擎 + 龟图 —— 单元测试（GL-free、纯 CPU）。
cc_test(
    name = "lsystem_test",
    srcs = ["lsystem_test.cc"],
    deps = [
        "//tools/jpov/src/vegetation:lsystem",
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

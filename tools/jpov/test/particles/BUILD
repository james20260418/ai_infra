load("@rules_cc//cc:defs.bzl", "cc_test")

# 广告牌粒子火 模拟内核单测（GL-free）。
cc_test(
    name = "particle_system_test",
    srcs = ["particle_system_test.cc"],
    deps = [
        "//tools/jpov/src/particles:particle_system",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

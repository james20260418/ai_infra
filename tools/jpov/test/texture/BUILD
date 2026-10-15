load("@rules_cc//cc:defs.bzl", "cc_test")

# 覆盖率保持 alpha mip 链 —— 单元测试（GL-free、纯 CPU）。
cc_test(
    name = "coverage_mipmap_test",
    srcs = ["coverage_mipmap_test.cc"],
    deps = [
        "//tools/jpov/src:coverage_mipmap",
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

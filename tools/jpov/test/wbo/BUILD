load("@rules_cc//cc:defs.bzl", "cc_test")

# WBO 权重函数「文本契约」：wbo_shader.h 与 object3d_renderer.h 两份副本必须一致。
# 只读字符串、不需要 GL context。
cc_test(
    name = "wbo_weight_contract_test",
    srcs = ["wbo_weight_contract_test.cc"],
    deps = [
        "//tools/jpov/src:object3d_renderer",
        "//tools/jpov/src/wbo:wbo_shader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

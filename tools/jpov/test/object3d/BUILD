load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["//tools/jpov/test:test_utils"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

cc_binary(
    name = "jpov_object3d_gold_generator",
    srcs = ["jpov_object3d_gold_generator.cc"],
    data = [":beetle.obj"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_object3d_gold_test",
    srcs = ["jpov_object3d_gold_test.cc"],
    data = [
        ":object3d_beetle_1280x720.png",
        ":beetle.obj",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

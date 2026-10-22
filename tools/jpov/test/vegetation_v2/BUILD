# vegetation v2 查看器（植物组合查看器）的 gold 保护。
#
# 场景参数在 vegetation_v2_gold_common.h，generator 与 test **共用同一份**，
# 且都直接驱动查看器 app（vegetation_v2_app.h）—— 测的就是用户看到的那条路径。

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_vegetation_v2_gold_generator",
    srcs = [
        "jpov_vegetation_v2_gold_generator.cc",
        "vegetation_v2_gold_common.h",
    ],
    data = ["//tools/jpov/tools/vegetation_tool_v2:sample_assets"],
    deps = [
        "//tools/jpov/test:test_utils",
        "//tools/jpov/tools/vegetation_tool_v2:vegetation_v2_app",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_vegetation_v2_gold_test",
    srcs = [
        "jpov_vegetation_v2_gold_test.cc",
        "vegetation_v2_gold_common.h",
    ],
    data = [
        "vegetation_v2_cards_1280x720.png",
        "vegetation_v2_leaf_1280x720.png",
        "//tools/jpov/tools/vegetation_tool_v2:sample_assets",
    ],
    deps = [
        "//tools/jpov/test:compare_light",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/tools/vegetation_tool_v2:vegetation_v2_app",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

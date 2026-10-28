# vegetation v2 查看器（植物组合查看器）的 gold 保护。
#
# gold 由**查看器二进制本身**跑出来（subprocess + --capture），test 与 generator 共用
# 同一份 vegetation_v2_gold_common.h。为什么不 in-process 驱动 app：
# 同一份 header 会被两个不同编译选项的 target 各编译一次，叶卡的 alpha 覆盖率 mip
# （CPU 侧二值化阈值）会因浮点细微差别翻转 texel ⇒ gold 与用户实际看到的图稳定差 12%。
# gold 是给人肉眼验收的，就必须等于用户看到的那张图。

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_vegetation_v2_gold_generator",
    srcs = [
        "jpov_vegetation_v2_gold_generator.cc",
        "vegetation_v2_gold_common.h",
    ],
    data = [
        "//tools/jpov/tools/vegetation_tool_v2:jpov_vegetation_v2_viewer",
        "//tools/jpov/tools/vegetation_tool_v2:sample_assets",
    ],
    deps = [
        "//tools/jpov/test:test_utils",
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
        "vegetation_v2_glb_1280x720.png",
        "vegetation_v2_leaf_1280x720.png",
        "vegetation_v2_pine_game_1280x720.png",
        "//tools/jpov/tools/vegetation_tool_v2:jpov_vegetation_v2_viewer",
        "//tools/jpov/tools/vegetation_tool_v2:sample_assets",
    ],
    deps = [
        "//tools/jpov/test:compare_light",
        "//tools/jpov/test:test_utils",
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

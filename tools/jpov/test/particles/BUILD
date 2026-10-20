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

# ============================================================================
# Gold Image — 火 + 烟 + 火星（固定种子 / 240 帧预演 / 1280x720）
# ============================================================================
cc_binary(
    name = "particle_gold_generator",
    srcs = ["particle_gold.cc"],
    data = ["//tools/jpov:particle_gold_assets"],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:fire_palette",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/particles:particle_system",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "particle_gold_test",
    srcs = ["particle_gold.cc"],
    data = [
        ":particle_fire_smoke_ember_1280x720.png",
        ":particle_fire_smoke_ember_soft_1280x720.png",
        "//tools/jpov:particle_gold_assets",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:fire_palette",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/particles:particle_system",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

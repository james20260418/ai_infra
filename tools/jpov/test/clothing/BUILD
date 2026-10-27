# tools/jpov/test/clothing — 穿衣工具 gold 测试（布料 LAG「裙摆飘起」回归保护）
#
# generator(cc_binary) 写仓库 gold PNG；test(cc_test) 渲一帧与 gold 比（ROI 均值）+ 断言
# 「开 LAG vs 关 LAG 画面明显不同」（裙摆确实飘起）。场景见 clothing_lag_gold_common.h。
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

_DATA = [
    "//tools/jpov/assets/models:mixamo_male_glb_data",
    "//tools/jpov/assets/models:clothing_data",
    "//tools/jpov/fonts:all_fonts",
]

cc_binary(
    name = "clothing_lag_gold_generator",
    srcs = [
        "clothing_lag_gold_generator.cc",
        "clothing_lag_gold_common.h",
    ],
    data = _DATA,
    deps = [
        "//tools/jpov/clothing:clothing_tool_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "clothing_lag_gold_test",
    srcs = [
        "clothing_lag_gold_test.cc",
        "clothing_lag_gold_common.h",
    ],
    data = _DATA + [":cloth_lag_640x360.png"],
    deps = [
        "//tools/jpov/clothing:clothing_tool_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# 顶点 tint gold —— tinted 裙子资产（_JPOV_TINT）三条路径 + 逐实例颜色
# ============================================================================
cc_binary(
    name = "clothing_tint_gold_generator",
    srcs = [
        "clothing_tint_gold_generator.cc",
        "clothing_tint_gold_common.h",
    ],
    data = _DATA,
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "clothing_tint_gold_test",
    srcs = [
        "clothing_tint_gold_test.cc",
        "clothing_tint_gold_common.h",
    ],
    data = _DATA + [
        ":cloth_tint_object3d_640x360.png",
        ":cloth_tint_instanced_640x360.png",
        ":cloth_tint_skinned_640x360.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

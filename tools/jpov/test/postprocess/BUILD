# JPOV 后处理（postprocess）验证 test。
# sRGB 编码是后处理链路的完整性保护（PR: sRGB 编码默认开启）。

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ============================================================================
# 共用：最终微调（final_contrast/brightness）场景构建（pliers 基底）。
# generator 与 test 共享，防场景参数分叉。
# ============================================================================
cc_library(
    name = "final_adjust_common",
    hdrs = ["final_adjust_common.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
    ],
)

# ============================================================================
# Test 1 — sRGB 编解码数学查表（纯 CPU，零渲染）
# 用 IEC 61966-2-1 标准查表值验证 LinearToSrgb / SrgbToLinear 公式正确性。
# ============================================================================
cc_test(
    name = "srgb_math_test",
    srcs = [
        "srgb.h",
        "srgb_math_test.cc",
    ],
    deps = [
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Test 2 — sRGB 编码在渲染链路上的行为锚点（需 headless 渲染）
# 用确定性纯色 3D 场景验证 srgb_encode=true 生效、方向正确、clamp 正常。
# ============================================================================
cc_test(
    name = "srgb_encode_anchor_test",
    srcs = [
        "srgb.h",
        "srgb_encode_anchor_test.cc",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Test 3 — 最终亮度/对比度微调 gold test（双图 + 像素范围检查）
# 以 pliers 场景为基底，验证 final_contrast/brightness 的方向与量级。
# ============================================================================
cc_binary(
    name = "final_adjust_gold_generator",
    srcs = [
        "final_adjust_gold_generator.cc",
        "final_adjust_common.h",
    ],
    deps = [
        ":final_adjust_common",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    data = ["//tools/jpov/test/object3d:pliers_gltf_data"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "final_adjust_gold_test",
    srcs = [
        "final_adjust_gold_test.cc",
        "final_adjust_common.h",
    ],
    deps = [
        ":final_adjust_common",
        "//tools/jpov/include/jpov:jpov",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    data = [
        ":final_adjust_highcontrast_lowbright.png",
        ":final_adjust_lowcontrast.png",
        "//tools/jpov/test/object3d:pliers_gltf_data",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

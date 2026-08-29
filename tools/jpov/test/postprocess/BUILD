# JPOV 后处理（postprocess）验证 test。
# sRGB 编码是后处理链路的完整性保护（PR: sRGB 编码默认开启）。

load("@rules_cc//cc:defs.bzl", "cc_test")

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

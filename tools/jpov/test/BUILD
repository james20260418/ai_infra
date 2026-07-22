load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# ============================================================================
# Gold Image Generator — 生成 test 参考图片
# ============================================================================
cc_binary(
    name = "jpov_gold_generator",
    srcs = ["jpov_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_circle_gold_generator",
    srcs = ["jpov_circle_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_polyline_gold_generator",
    srcs = ["jpov_polyline_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_text_gold_generator",
    srcs = ["jpov_text_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — rect 渲染验证单元测试
# ============================================================================
cc_test(
    name = "jpov_rect_gold_test",
    srcs = ["jpov_rect_gold_test.cc"],
    data = [":rect_centered_blue_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — polyline gold image 单元测试
# ============================================================================
cc_test(
    name = "jpov_polyline_gold_test",
    srcs = ["jpov_polyline_gold_test.cc"],
    data = [":polyline_zigzag_red_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — circle 渲染验证单元测试
# ============================================================================
cc_test(
    name = "jpov_circle_gold_test",
    srcs = ["jpov_circle_gold_test.cc"],
    data = [":circle_centered_green_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — text gold image 单元测试
# ============================================================================
cc_test(
    name = "jpov_text_gold_test",
    srcs = ["jpov_text_gold_test.cc"],
    data = [":hello_jpov_48_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

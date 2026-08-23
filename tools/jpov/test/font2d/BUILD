load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# ============================================================================
# Gold Image — Text 渲染验证（字体 2D）
# ============================================================================

cc_binary(
    name = "jpov_text_gold_generator",
    srcs = ["jpov_text_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

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

# ============================================================================
# Gold Image — CJK 文字渲染验证
# ============================================================================

cc_binary(
    name = "jpov_text_cjk_gold_generator",
    srcs = ["jpov_text_cjk_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_text_cjk_gold_test",
    srcs = ["jpov_text_cjk_gold_test.cc"],
    data = [":hello_cjk_36_640x360.png"],
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
# Gold Image — 中文渲染验证
# ============================================================================

cc_binary(
    name = "jpov_chinese_gold_generator",
    srcs = ["jpov_chinese_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_chinese_gold_test",
    srcs = ["jpov_chinese_gold_test.cc"],
    data = [":hello_chinese_jpov_1280x720.png"],
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
# 颜色/对齐渲染验证
# ============================================================================

cc_binary(
    name = "color_align_test",
    srcs = ["color_align_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

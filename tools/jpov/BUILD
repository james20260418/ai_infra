load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

exports_files(
    ["fonts/DejaVuSans.ttf"],
    visibility = ["//visibility:public"],
)

# ============================================================================
# 根 BUILD — 仅定义 cc_binary / cc_test target
# cc_library 均下沉到各自源路径下的独立 BUILD 文件
# ============================================================================

# ============================================================================
# 示例：Rect Demo — 演示窗口坐标与渲染分辨率裁剪
# ============================================================================
cc_binary(
    name = "jpov_demo",
    srcs = ["demo/jpov_demo.cc"],
    deps = ["//tools/jpov/include/jpov:jpov"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_demo.exe",
    srcs = ["demo/jpov_demo.cc"],
    deps = ["//tools/jpov/include/jpov:jpov"],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# 示例：Screenshot Demo — 绘制后保存 PNG 截图
# ============================================================================
cc_binary(
    name = "jpov_screenshot_demo",
    srcs = ["demo/jpov_screenshot_demo.cc"],
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
# 示例：Polyline Demo — 绘制后保存 PNG 截图
# ============================================================================
cc_binary(
    name = "jpov_polyline_demo",
    srcs = ["demo/jpov_polyline_demo.cc"],
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
    name = "jpov_polyline_verify",
    srcs = ["demo/jpov_polyline_verify.cc"],
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
# 示例：Text Demo — 验证 Text2D 渲染
# ============================================================================
cc_binary(
    name = "jpov_text_demo",
    srcs = ["demo/jpov_text_demo.cc"],
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
# Gold Image Generator — 生成 rect 测试的参考图片
# ============================================================================
cc_binary(
    name = "jpov_gold_generator",
    srcs = ["test/jpov_gold_generator.cc"],
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
    srcs = ["test/jpov_circle_gold_generator.cc"],
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
    srcs = ["test/jpov_rect_gold_test.cc"],
    data = ["test/rect_centered_blue_640x360.png"],
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
# Gold Image — polyline gold image 生成器 + 测试
# ============================================================================
cc_binary(
    name = "jpov_polyline_gold_generator",
    srcs = ["test/jpov_polyline_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_polyline_gold_test",
    srcs = ["test/jpov_polyline_gold_test.cc"],
    data = ["test/polyline_zigzag_red_640x360.png"],
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
    srcs = ["test/jpov_circle_gold_test.cc"],
    data = ["test/circle_centered_green_640x360.png"],
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
# Gold Image — text gold image 生成器 + 测试
# ============================================================================
cc_binary(
    name = "jpov_text_gold_generator",
    srcs = ["test/jpov_text_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_text_gold_test",
    srcs = ["test/jpov_text_gold_test.cc"],
    data = ["test/hello_jpov_48_640x360.png"],
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

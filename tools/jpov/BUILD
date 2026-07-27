load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# ============================================================================
# 根 BUILD — 仅定义 cc_binary / cc_test target
# cc_library 均下沉到各自源路径下的独立 BUILD 文件
# 字体文件: 见 //tools/jpov/fonts:all_fonts
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

# 见 //tools/jpov/test:BUILD — gold generator + gold test 均移至 test/BUILD 管理

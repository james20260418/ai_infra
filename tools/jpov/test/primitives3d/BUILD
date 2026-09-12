load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

# ============================================================================
# Gold Image — 3D 正方体渲染验证（图元三角形）
# ============================================================================

cc_binary(
    name = "jpov_cube3d_gold_generator",
    srcs = ["jpov_cube3d_gold_generator.cc"],
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
    name = "jpov_cube3d_gold_test",
    srcs = ["jpov_cube3d_gold_test.cc"],
    data = [":cube3d_6faces_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — 3D 条带渲染验证（GL_TRIANGLE_STRIP）
# ============================================================================

cc_binary(
    name = "jpov_strip3d_gold_generator",
    srcs = ["jpov_strip3d_gold_generator.cc"],
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
    name = "jpov_strip3d_gold_test",
    srcs = ["jpov_strip3d_gold_test.cc"],
    data = [":strip3d_ring_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — 3D 文本渲染验证（空间 quad + 字形 atlas，无光照纯色）
#
# 除 gold 字节比对外，还跑纯几何门禁（BuildText3DWorldVerts 朝向/尺寸/
# 正交化 + PixelsPerMeterAt 解析解）—— 单靠 gold 抓不到镜像/方向反转。
# ============================================================================

cc_binary(
    name = "jpov_text3d_gold_generator",
    srcs = ["jpov_text3d_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    data = ["//tools/jpov/fonts:all_fonts"],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_text3d_gold_test",
    srcs = ["jpov_text3d_gold_test.cc"],
    data = [
        ":text3d_quad_1280x720.png",
        "//tools/jpov/fonts:all_fonts",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:text3d_util",
        "//tools/jpov/src:primitives3d_renderer",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)


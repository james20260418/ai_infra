load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# 半透明条带 CPU 侧几何/校验单测（GL-free，无需 GL context）。
cc_test(
    name = "strip_geometry_test",
    srcs = ["strip_geometry_test.cc"],
    deps = [
        "//tools/jpov/src/strip:strip_geometry",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# ============================================================================
# Gold Image — 三条带 + 天光 + 参考方块（固定相机 / 1280x720）
# ============================================================================
cc_binary(
    name = "strip_gold_generator",
    srcs = ["strip_gold.cc"],
    data = [
        "//tools/jpov:strip_gold_assets",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:strip_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# Gold Image — viewer 全家福（三条带 + 圆环 + 射流 + fresnel + bloom）
#
# 重构（Strip16 收口 per-instance 属性）的 no-op 护栏：重构前先锁 8 张固定构图，
# 重构后逐字节一致 = 通道没被改坏。刻意与 strip_gold 分开（那份锁 M1~M3 折线路径）。
# ============================================================================
cc_binary(
    name = "strip_scene_gold_generator",
    srcs = ["strip_scene_gold.cc"],
    data = [
        "//tools/jpov:strip_gold_assets",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:strip_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "strip_scene_gold_test",
    srcs = ["strip_scene_gold.cc"],
    data = glob(["strip_scene_*.png"]) + [
        "//tools/jpov:strip_gold_assets",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:strip_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "strip_gold_test",
    srcs = ["strip_gold.cc"],
    data = [
        ":strip_demo_1280x720.png",
        ":strip_additive_soft_1280x720.png",
        "//tools/jpov:strip_gold_assets",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:strip_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

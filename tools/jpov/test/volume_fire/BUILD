load("@rules_cc//cc:defs.bzl", "cc_test")

# ============================================================================
# Gold Image — 体积火（raymarch）+ 入库的篝火样例资产
#   * 逐字节比对 1280x720 渲染；
#   * 另加非恒真断言：火焰像素数必须落在区间内（防资产退化成"发光棒"）。
# ============================================================================
cc_binary(
    name = "volume_fire_gold_generator",
    srcs = ["volume_fire_gold.cc"],
    data = [
        "//data/fire_vdb:campfire.jpovvol",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:skylight_scene",
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
    name = "volume_fire_gold_test",
    srcs = ["volume_fire_gold.cc"],
    data = [
        ":campfire_1280x720.png",
        "//data/fire_vdb:campfire.jpovvol",
    ],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov:skylight_scene",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

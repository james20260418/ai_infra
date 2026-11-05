load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# Fire-Fog froxel gold（体积雾 + CSM god ray）—— 共享场景/渲染体（header-only），
# generator 与 test 共用，防参数分叉。
cc_library(
    name = "fog_gold_common",
    hdrs = ["fog_gold_common.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov:skylight_scene",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# gold generator —— 写仓库 gold image（fog_godray_1280x720.png）。
cc_binary(
    name = "jpov_fog_gold_generator",
    srcs = [
        "fog_gold_generator.cc",
        "fog_gold_common.h",
    ],
    data = ["//tools/jpov/assets/models:oak_tripo_negative_data"],
    deps = [
        ":fog_gold_common",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# gold test —— 渲染 + 与 gold 做 8×8 ROI 平均色比对。
cc_test(
    name = "jpov_fog_gold_test",
    srcs = [
        "jpov_fog_gold_test.cc",
        "fog_gold_common.h",
    ],
    data = [
        "//tools/jpov/assets/models:oak_tripo_negative_data",
        ":fog_godray_1280x720.png",
    ],
    deps = [
        ":fog_gold_common",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 点光源雾散射 gold generator —— 写仓库 gold image（fog_pointlight_1280x720.png）。
cc_binary(
    name = "jpov_fog_pointlight_gold_generator",
    srcs = [
        "fog_pointlight_gold_generator.cc",
        "fog_gold_common.h",
    ],
    data = ["//tools/jpov/assets/models:oak_tripo_negative_data"],
    deps = [
        ":fog_gold_common",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 点光源雾散射 gold test —— 渲染 + 与 gold 做 8×8 ROI 平均色比对。
cc_test(
    name = "jpov_fog_pointlight_gold_test",
    srcs = [
        "jpov_fog_pointlight_gold_test.cc",
        "fog_gold_common.h",
    ],
    data = [
        "//tools/jpov/assets/models:oak_tripo_negative_data",
        ":fog_pointlight_1280x720.png",
    ],
    deps = [
        ":fog_gold_common",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:gltf_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

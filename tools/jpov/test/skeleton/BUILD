# tools/jpov/test/skeleton — 骨架蒙皮渲染 gold 测试(M1:单 pose T-rest 静态)
#
# 独立目录：单独 shader(蒙皮)路径。generator/test 共用 jpov_skeleton_gold_common.h 场景
# （地面 + sunny-day + 左蒙皮人/右 object3d 人）。
# generator(cc_binary) 写仓库 gold PNG；test(cc_test) 渲一帧与 gold 逐像素 diff。
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_skeleton_gold_generator",
    srcs = [
        "jpov_skeleton_gold_generator.cc",
        "jpov_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        "//tools/jpov/test/object3d:scene_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src/skeleton:skeleton",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_skeleton_gold_test",
    srcs = [
        "jpov_skeleton_gold_test.cc",
        "jpov_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        "//tools/jpov/test/object3d:scene_assets",
        ":human_skeleton_gold_t_rest_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src/skeleton:skeleton",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

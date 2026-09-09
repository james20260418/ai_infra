# tools/jpov/test/skeleton — 骨架蒙皮渲染 gold 测试（M1：单 pose T-rest 静态）
#
# 独立目录：这是单独 shader（蒙皮）路径，不复用 object3d 的 test 目录。
# generator(cc_binary) 写仓库 gold image (human_skeleton_gold_t_rest_1280x720.png)，
# test(cc_test) 渲染一帧 + 校验 gold 存在 + 非黑 smoke。
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_skeleton_gold_generator",
    srcs = ["jpov_skeleton_gold_generator.cc"],
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
    srcs = ["jpov_skeleton_gold_test.cc"],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
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
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

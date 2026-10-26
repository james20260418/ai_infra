# 植被风（单关节）gold —— instance pass 与 object3d 路径的顶点风回归。
#
# 场景与 shot 清单在 vegetation_wind_gold_common.h（generator / test 共用一份），
# 资产是**现场用 v2 素材烘**的树干（无叶子：alpha cutout 在 llvmpipe 下当不了逐像素基准）。
#
# 出新基准（只有故意改风公式/观感时才跑，跑完把三张 png 一起提交）：
#   bazel run //tools/jpov/test/vegetation_wind:vegetation_wind_gold_generator

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "vegetation_wind_gold_generator",
    srcs = [
        "vegetation_wind_gold_common.h",
        "vegetation_wind_gold_generator.cc",
    ],
    data = [
        "//tools/jpov/tools/vegetation_tool_v2:sample_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
        "//tools/jpov/tools/vegetation_tool_v2:vegetation_glb_baker",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "vegetation_wind_gold_test",
    srcs = [
        "vegetation_wind_gold_common.h",
        "vegetation_wind_gold_test.cc",
    ],
    data = [
        "vegetation_wind_instance_1280x720.png",
        "vegetation_wind_instance_blow_1280x720.png",
        "vegetation_wind_object3d_1280x720.png",
        "//tools/jpov/tools/vegetation_tool_v2:sample_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/test:compare_light",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
        "//tools/jpov/tools/vegetation_tool_v2:vegetation_glb_baker",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

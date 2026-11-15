load("@rules_cc//cc:defs.bzl", "cc_test")

# 水皮 CPU 单测（GL-free）：临界角 / 菲涅尔 / 平面交点 / 深度拒绝。
cc_test(
    name = "water_skin_math_test",
    srcs = ["water_skin_math_test.cc"],
    deps = [
        "//tools/jpov/src/water:water_skin_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水皮 GPU 像素测试（headless GL / llvmpipe）：与 CPU 参考模型逐像素对拍，
# 外加 WBO 三个附件的语义（accum / reveal / att3）与深度拒绝。
# 不需要场景/物体/相机矩阵：水皮的输入只有 env cubemap + 场景深度 + 相机。
cc_test(
    name = "water_skin_gpu_test",
    srcs = ["water_skin_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/water:water_skin_math",
        "//tools/jpov/src/water:water_skin_renderer",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

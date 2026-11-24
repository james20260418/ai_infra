load("@rules_cc//cc:defs.bzl", "cc_test")

# 水体介质的 CPU 单测（GL-free）：闭式 vs 数值积分、段裁剪、逐通道吸收、折射方向。
cc_test(
    name = "water_medium_math_test",
    srcs = ["water_medium_math_test.cc"],
    deps = [
        "//tools/jpov/interface:render_command",
        "//tools/jpov/src/water:water_medium_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水体介质的 GPU 像素测试：合成趟（逐通道透射的两次 draw）与 CPU 参考模型逐像素对拍。
cc_test(
    name = "water_medium_gpu_test",
    srcs = ["water_medium_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/fog:fog_composite_renderer",
        "//tools/jpov/src/water:water_medium_math",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 水皮 GPU 像素测试（headless GL / llvmpipe）：层语义（C / α / 射线距离）与 CPU 参考
# 逐像素对拍；水下环外/环内、水上 ÷n²、深度拒绝四条。
cc_test(
    name = "water_skin_gpu_test",
    srcs = ["water_skin_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/lighting:ggx_specular",
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

# 水面波浪 CPU 单测（GL-free）：无波零回归 / 单波解析式 / 梯度数值差分 /
# flow 调制 / 淡出 / 求交残差。
cc_test(
    name = "water_wave_math_test",
    srcs = ["water_wave_math_test.cc"],
    deps = [
        "//tools/jpov/src/water:water_wave_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 水皮 CPU 单测（GL-free）：临界角 / 菲涅尔 / 平面交点 / 深度拒绝。
cc_test(
    name = "water_skin_math_test",
    srcs = ["water_skin_math_test.cc"],
    deps = [
        "//tools/jpov/src/lighting:ggx_specular",
        "//tools/jpov/src/water:water_skin_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# WaterConfig → 谱的展开（纯 CPU）：SMB 的 Hs/Tp、方差精确、seed 确定性、
# 无风镜面、涌按能量叠加、质量档、与渲染侧同源常数的一致性、浅水色散。
cc_test(
    name = "ocean_state_test",
    srcs = ["ocean_state_test.cc"],
    deps = [
        "//tools/jpov/interface:water_config",
        "//tools/jpov/src/water:water_wave_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)


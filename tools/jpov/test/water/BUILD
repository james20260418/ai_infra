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

# 水皮 gold 生成器（bazel run）：直接驱动 WaterSkinRenderer 拍 6 张固定机位
# （水上 ×3 / 水下 ×3），写 tools/jpov/test/water/water_skin_gold_*_1280x720.png。
# 机位 / 输入定义见 jpov_water_skin_gold_common.h。
cc_binary(
    name = "jpov_water_skin_gold_generator",
    srcs = [
        "jpov_water_skin_gold_common.h",
        "jpov_water_skin_gold_generator.cc",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/lighting:ggx_specular",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/water:water_skin_renderer",
        "//tools/jpov/src/water:water_medium_math",
        "//tools/jpov/test:test_utils",
        "//third_party/stb:stb_image_write",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 水皮 gold 测试（bazel test）：对拆分前后的水皮逐像素比对（双闸门：ROI 均值 + 像素变化）。
# 水皮是确定性单 pass，无 llvmpipe 抖动 ⇒ 闸门比 SSR 紧得多（LSB 级）。
cc_test(
    name = "jpov_water_skin_gold_test",
    srcs = [
        "jpov_water_skin_gold_common.h",
        "jpov_water_skin_gold_test.cc",
    ],
    data = glob(["water_skin_gold_*_1280x720.png"]),
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/lighting:ggx_specular",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/water:water_skin_renderer",
        "//tools/jpov/src/water:water_medium_math",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
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

# 水皮 V2（单像素多法线采样）GPU 测试：平面等价性（V2 == V1 逐像素）+ 采样是否真的在生效
# + S 越大空间方差越小。见 water_skin_v2_gpu_test.cc 顶部。
cc_test(
    name = "water_skin_v2_gpu_test",
    srcs = ["water_skin_v2_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/lighting:ggx_specular",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/water:water_skin_renderer",
        "//tools/jpov/src/water:water_skin_v2_renderer",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 水皮 SSR 的 GPU 测试：query 趟的 depth/法线口径 + A=0 逐字节等于纯探针。
# 见 water_ssr_gpu_test.cc 顶部（含三条负向验证的做法）。
cc_test(
    name = "water_ssr_gpu_test",
    srcs = ["water_ssr_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/water:water_skin_v2_renderer",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 水下照明衰减（照明侧）的 CPU 参考单测：逐通道增益、开关独立、水上恒等、低太阳更暗。
cc_test(
    name = "water_underwater_shading_test",
    srcs = ["water_underwater_shading_test.cc"],
    deps = [
        "//tools/jpov/src/water:water_underwater_shading",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

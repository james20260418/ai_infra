load("@rules_cc//cc:defs.bzl", "cc_test")

# SSR 算法数学单测（GL-free，无窗口、无 GL）。
# 钉住整套 SSR 仅有的两条公理（1/d 对屏幕弧长线性、体素柱区间重叠判据）与
# 分段并行取 min ≡ 单线程 DDA 的等价性；每条断言都带负向对照。
cc_test(
    name = "ssr_math_test",
    srcs = ["ssr_math_test.cc"],
    deps = [
        "//tools/jpov/src/ssr:ssr_math",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SSR GPU 像素测试（headless GL / llvmpipe）：合成三张输入纹理，直接驱动 SsrRenderer。
# 三板斧：CPU 参考对拍、手推的命中/出屏模式、tile 1/2/4/8 逐字节等价。
cc_test(
    name = "ssr_gpu_test",
    srcs = ["ssr_gpu_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/ssr:ssr_math",
        "//tools/jpov/src/ssr:ssr_renderer",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# SSR 输入准备（降采样）的纯数学单测：倍率校验、目标尺寸、N×N 选点规则。
cc_test(
    name = "ssr_downsample_test",
    srcs = ["ssr_downsample_test.cc"],
    deps = [
        "//tools/jpov/src/ssr:ssr_downsample",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SSR 粗糙度预滤波（A 预乘 mip 链）的 GPU 单测：覆盖率加权 / 空像素填充 / 覆盖率随 mip 下降。
cc_test(
    name = "ssr_filter_test",
    srcs = ["ssr_filter_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src/ssr:ssr_filter",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# SSR 端到端 gold：天光查看器「场景1（镜面地板）」的 5 个固定组合
# （掠射 / 1:1 / 金属磨砂 / 四段拼接 / 低太阳 + 1:3）。组合定义见
# jpov_ssr_gold_common.h；验收闸门见 jpov_ssr_gold_test.cc。
cc_binary(
    name = "jpov_ssr_gold_generator",
    srcs = [
        "jpov_ssr_gold_common.h",
        "jpov_ssr_gold_generator.cc",
    ],
    data = [
        "//tools/jpov/assets/models:oak_tripo_negative_data",
        "//tools/jpov/assets/models:mixamo_male_glb_data",
        "//tools/jpov/assets/models:scene_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:skylight_viewer_app",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/test:test_utils",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_ssr_gold_test",
    srcs = [
        "jpov_ssr_gold_common.h",
        "jpov_ssr_gold_test.cc",
    ],
    data = glob(["ssr_gold_*_1280x720.png"]) + [
        "//tools/jpov/assets/models:oak_tripo_negative_data",
        "//tools/jpov/assets/models:mixamo_male_glb_data",
        "//tools/jpov/assets/models:scene_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:skylight_viewer_app",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
        "//third_party/stb:stb_image",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

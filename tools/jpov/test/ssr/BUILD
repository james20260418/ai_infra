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

load("@rules_cc//cc:defs.bzl", "cc_test")

# GGX 单次散射方向反照率 E_ss / 多散射补偿（M2）的单测（GL-free）：
# 表 vs 独立数值积分、物理边界（α=1,μ=1 ⇒ 0.31）、单调性、F_avg 自洽。
cc_test(
    name = "ggx_albedo_test",
    srcs = ["ggx_albedo_test.cc"],
    deps = [
        "//tools/jpov/src/lighting:ggx_specular",
        "//tools/jpov/src/water:water_optics",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

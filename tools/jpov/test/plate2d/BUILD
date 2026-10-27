load("@rules_cc//cc:defs.bzl", "cc_test")

# Plate2D GPU 像素测试（需 GL；CI 上跑 llvmpipe 即可，纯色断言不依赖精度）。
# 锁住 CPU 指令层测不到的着色器行为：图集行序、fps=0 静止在第 0 格、
# 末帧回绕混合、两层材质独立。
cc_test(
    name = "plate2d_gold_test",
    srcs = ["plate2d_gold_test.cc"],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/common:utils",
        "//tools/jpov/include/jpov:jpov",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

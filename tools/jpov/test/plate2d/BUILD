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
    # 独占：headless 的 GL 上下文靠 JPOV 自启 Xvfb on :99（见 jpov.cc Init）。
    # 多个 GL 测试并行会抢 :99 —— 抢输的那个会打 "Fatal server error"，
    # 目前靠「连上已经存在的 :99」侥幸能过（实测 9/9 绿），但那是运气。
    # 本测试独占跑，至少不再由它制造争用（仓库其它 GL 测试仍建议
    # --local_test_jobs=1，这是既有已知问题）。
    tags = ["exclusive"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

load("@rules_cc//cc:defs.bzl", "cc_library")

# GGX 镜面高光的共享实现（header-only）：CPU 参考 + GLSL 片段（kGgxSpecularGlsl）。
# 目前消费者 = 水皮（太阳的像）；下一步 PBR 的三份片元也改用它（见头注释「待统一」）。
cc_library(
    name = "ggx_specular",
    hdrs = ["ggx_specular.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

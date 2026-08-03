package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "stb_image_write",
    hdrs = ["stb_image_write.h"],
    includes = ["."],
    linkstatic = 1,
    copts = ["-DSTB_IMAGE_WRITE_IMPLEMENTATION"],
)

cc_library(
    name = "stb_truetype",
    hdrs = ["stb_truetype.h"],
    includes = ["."],
    linkstatic = 1,
    copts = ["-DSTB_TRUETYPE_IMPLEMENTATION"],
)

cc_library(
    name = "stb_image",
    srcs = ["stb_image_impl.c"],
    hdrs = ["stb_image.h"],
    includes = ["."],
    linkstatic = 1,
)



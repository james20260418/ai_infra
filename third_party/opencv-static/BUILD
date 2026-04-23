package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "opencv",
    srcs = [
        "lib/libopencv_world.a",
        "lib/liblibjpeg.a",
        "lib/liblibpng.a",
        "lib/liblibtiff.a",
        "lib/liblibjasper.a",
        "lib/liblibwebp.a",
        "lib/libippiw.a",
        "lib/libittnotify.a",
    ],
    hdrs = glob(["include/**/*.hpp", "include/**/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    alwayslink = 1,
    linkopts = [
        "-lpthread",
        "-ldl",
        "-lm",
        "-lz",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_library")

# tinygltf — header-only glTF 2.0 loader (MIT)
# 编译单元约定：一个 cc 文件 #define TINYGLTF_IMPLEMENTATION 并 include 本头文件
cc_library(
    name = "tinygltf",
    hdrs = [
        "json.hpp",
        "tiny_gltf.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//third_party/stb:stb_image",
    ],
)

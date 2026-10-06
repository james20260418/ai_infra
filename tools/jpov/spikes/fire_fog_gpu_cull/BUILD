load("@rules_cc//cc:defs.bzl", "cc_binary")

# Fire-Fog tile culling spike（临时，验证后删整个目录）
# 对比「GPU 小 FBO 建表（K 趟 GL_MIN）」vs「CPU SAT/AABB」，含正确性对照。
cc_binary(
    name = "fire_fog_gpu_cull",
    srcs = ["fire_fog_gpu_cull.cc"],
    deps = [
        "@glog_static//:glog",
        "@glfw_static//:glfw",
    ],
    copts = ["-std=c++20"],
    linkopts = ["-lGL"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

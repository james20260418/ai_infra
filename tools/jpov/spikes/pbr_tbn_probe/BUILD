load("@rules_cc//cc:defs.bzl", "cc_binary")

# 阶段0 TBN 探针（临时 spike，验证后删除整个目录）
# 验证 GL 环境能否运行 RGBA8 法线贴图 + TBN 管线。
cc_binary(
    name = "pbr_tbn_probe",
    srcs = ["pbr_tbn_probe.cc"],
    deps = [
        "//third_party/stb:stb_image_write",
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

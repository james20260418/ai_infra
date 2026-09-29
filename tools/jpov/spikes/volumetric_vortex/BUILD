load("@rules_cc//cc:defs.bzl", "cc_binary")

# 预实验 spike（验证后删除整个目录）：
# 「每颗粒子 = 一个涡流体」的体积渲染——相机朝向包围 quad +
# 逐像素 ray-march 程序化 3D 密度场（旋涡 + 域扭曲），按场景深度裁剪。
cc_binary(
    name = "volumetric_vortex",
    srcs = ["volumetric_vortex.cc"],
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

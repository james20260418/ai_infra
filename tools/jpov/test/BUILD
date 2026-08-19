load("@rules_cc//cc:defs.bzl", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

# 光照 gold image 比对工具：自动平铺 ROI + 块内平均颜色对比，输出最大 RGB 通道差。
# header-only，无自定义实现，仅依赖 stb_image（调用方需已 link）与 glog。
cc_library(
    name = "compare_light",
    hdrs = ["compare_light.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = [
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
)

# ============================================================================
# OBJ 加载器单元测试 — 纯 CPU 解析（不渲染）
# ============================================================================
cc_test(
    name = "jpov_obj_loader_test",
    srcs = ["jpov_obj_loader_test.cc"],
    data = [
        ":beetle.obj",
        ":cube_hand.obj",
        ":suzanne.obj",
    ],
    deps = [
        "//tools/jpov/interface:mesh",
        "//tools/jpov/src:obj_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# glTF loader smoke test (CPU-only parse check)
cc_test(
    name = "jpov_gltf_loader_test",
    srcs = ["jpov_gltf_loader_test.cc"],
    data = ["//tools/jpov/test/object3d:pliers_gltf_data"],
    deps = [
        "//tools/jpov/src:gltf_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

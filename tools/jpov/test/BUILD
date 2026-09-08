load("@rules_cc//cc:defs.bzl", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = [
        "//tools/common:utils",
        "@glog_static//:glog",
    ],
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

# glTF loader — 内嵌 bufferView 贴图 GLB 加载验证（CPU-only）
# 确保单文件 GLB（贴图内嵌、无外部 uri）能正确导出贴图并填入 *_tex。
cc_test(
    name = "jpov_gltf_embedded_loader_test",
    srcs = ["jpov_gltf_embedded_loader_test.cc"],
    data = ["//tools/jpov/test/object3d:catapult_glb_data"],
    deps = [
        "//tools/jpov/src:gltf_loader",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# FBX (Mixamo 动画) 骨骼动画源 → FBXClip 加载验证（CPU-only, 真实 dance.fbx 资材自洽性）
cc_test(
    name = "jpov_fbx_loader_test",
    srcs = ["jpov_fbx_loader_test.cc"],
    data = ["//tools/jpov/test/animations:hip_hop_dance_fbx_data"],
    deps = [
        "//tools/jpov/src:fbx_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# glTF loader — 骨骼（JOINTS_0/WEIGHTS_0 顶点侧 + LoadGltfSkeleton 关节树/逆绑定）验证（CPU-only）
# 真实资材 mixamo_male.glb（Tripo Auto Rig spec=mixamo, 23 关节带蒙皮）。
cc_test(
    name = "jpov_gltf_skeleton_loader_test",
    srcs = ["jpov_gltf_skeleton_loader_test.cc"],
    data = ["//tools/jpov/test/object3d:mixamo_male_glb_data"],
    deps = [
        "//tools/jpov/src:gltf_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

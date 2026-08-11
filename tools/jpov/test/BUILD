load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
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

# ============================================================================
# Gold Image — PBR baseColor 纹理采样渲染验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_basecolor_gold_generator",
    srcs = ["jpov_pbr_basecolor_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_pbr_basecolor_gold_test",
    srcs = ["jpov_pbr_basecolor_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — PBR 法线映射（normal map TBN）渲染验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_normal_gold_generator",
    srcs = ["jpov_pbr_normal_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_pbr_normal_gold_test",
    srcs = ["jpov_pbr_normal_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — PBR Emissive 自发光贴图验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_emissive_gold_generator",
    srcs = ["jpov_pbr_emissive_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
        ":pbr_emissive_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_pbr_emissive_gold_test",
    srcs = ["jpov_pbr_emissive_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
        ":pbr_emissive_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — PBR AO 环境光遮蔽验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_ao_gold_generator",
    srcs = ["jpov_pbr_ao_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
        ":pbr_emissive_256x256.png",
        ":pbr_ao_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_pbr_ao_gold_test",
    srcs = ["jpov_pbr_ao_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_normal_256x256.png",
        ":pbr_emissive_256x256.png",
        ":pbr_ao_256x256.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

cc_binary(
    name = "color_align_test",
    srcs = ["color_align_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image Generator — 生成 test 参考图片
# ============================================================================
cc_binary(
    name = "jpov_gold_generator",
    srcs = ["jpov_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_circle_gold_generator",
    srcs = ["jpov_circle_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_polyline_gold_generator",
    srcs = ["jpov_polyline_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_text_gold_generator",
    srcs = ["jpov_text_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_text_cjk_gold_generator",
    srcs = ["jpov_text_cjk_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_cube3d_gold_generator",
    srcs = ["jpov_cube3d_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_chinese_gold_generator",
    srcs = ["jpov_chinese_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_strip3d_gold_generator",
    srcs = ["jpov_strip3d_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_strip2d_gold_generator",
    srcs = ["jpov_strip2d_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_roundrect_gold_generator",
    srcs = ["jpov_roundrect_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_fillrect_gold_generator",
    srcs = ["jpov_fillrect_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_arc2d_gold_generator",
    srcs = ["jpov_arc2d_gold_generator.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_image2d_gold_generator",
    srcs = ["jpov_image2d_gold_generator.cc"],
    data = [":stamp_200x200.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — rect 渲染验证单元测试
# ============================================================================
cc_test(
    name = "jpov_rect_gold_test",
    srcs = ["jpov_rect_gold_test.cc"],
    data = [":rect_centered_blue_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — polyline gold image 单元测试
# ============================================================================
cc_test(
    name = "jpov_polyline_gold_test",
    srcs = ["jpov_polyline_gold_test.cc"],
    data = [":polyline_zigzag_red_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — circle 渲染验证单元测试
# ============================================================================
cc_test(
    name = "jpov_circle_gold_test",
    srcs = ["jpov_circle_gold_test.cc"],
    data = [":circle_centered_green_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# ============================================================================
# Gold Image — text gold image 单元测试
# ============================================================================
cc_test(
    name = "jpov_text_gold_test",
    srcs = ["jpov_text_gold_test.cc"],
    data = [":hello_jpov_48_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_text_cjk_gold_test",
    srcs = ["jpov_text_cjk_gold_test.cc"],
    data = [":hello_cjk_36_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_chinese_gold_test",
    srcs = ["jpov_chinese_gold_test.cc"],
    data = [":hello_chinese_jpov_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_cube3d_gold_test",
    srcs = ["jpov_cube3d_gold_test.cc"],
    data = [":cube3d_6faces_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_strip3d_gold_test",
    srcs = ["jpov_strip3d_gold_test.cc"],
    data = [":strip3d_ring_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_strip2d_gold_test",
    srcs = ["jpov_strip2d_gold_test.cc"],
    data = [":strip2d_diagonal_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_roundrect_gold_test",
    srcs = ["jpov_roundrect_gold_test.cc"],
    data = [":roundrect_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_fillrect_gold_test",
    srcs = ["jpov_fillrect_gold_test.cc"],
    data = [":fillrect_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        ":test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_test(
    name = "jpov_arc2d_gold_test",
    srcs = ["jpov_arc2d_gold_test.cc"],
    data = [":arc2d_640x360.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
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
# Gold Image — image2d gold image 单元测试（PNG 纹理渲染）
# ============================================================================
cc_test(
    name = "jpov_image2d_gold_test",
    srcs = ["jpov_image2d_gold_test.cc"],
    data = [
        ":image2d_stamp_640x360.png",
        ":stamp_200x200.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
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
# Gold Image — Object3D (RegisterMesh→DrawObject3D 链路) 渲染验证
# ============================================================================
cc_binary(
    name = "jpov_repro_purecolor_cube",
    srcs = ["jpov_repro_purecolor_cube.cc"],
    data = [":cube_hand.obj"],
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

cc_binary(
    name = "jpov_object3d_gold_generator",
    srcs = ["jpov_object3d_gold_generator.cc"],
    data = [":beetle.obj"],
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
    name = "jpov_object3d_gold_test",
    srcs = ["jpov_object3d_gold_test.cc"],
    data = [
        ":object3d_beetle_1280x720.png",
        ":beetle.obj",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
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
# PBRMaterial 单元测试 — 纯 CPU 数据结构语义（构建 + fallback，不进 gold）
# ============================================================================
cc_test(
    name = "jpov_pbr_material_test",
    srcs = ["jpov_pbr_material_test.cc"],
    deps = [
        "//tools/jpov/interface:render_command",
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
# Gold Image — 3D 点光源 GGX PBR 光照渲染验证
# ============================================================================
cc_binary(
    name = "jpov_lighting_gold_generator",
    srcs = ["jpov_lighting_gold_generator.cc"],
    data = [":beetle.obj"],
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
    name = "jpov_lighting_gold_test",
    srcs = ["jpov_lighting_gold_test.cc"],
    data = [
        ":lighting_beetle_1280x720.png",
        ":beetle.obj",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
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
# Gold Image — PBR 多通道纹理采样渲染验证
# （baseColor/metallic/roughness/emissive/AO 逐个 sampler+开关进 BRDF）
# ============================================================================
cc_binary(
    name = "jpov_pbr_textures_gold_generator",
    srcs = ["jpov_pbr_textures_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_metallic_256x256.png",
        ":pbr_roughness_256x256.png",
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
    name = "jpov_pbr_textures_gold_test",
    srcs = ["jpov_pbr_textures_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_metallic_256x256.png",
        ":pbr_roughness_256x256.png",
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

# ============================================================================
# Gold Image — PBR 法线映射（normal map TBN）渲染验证
# 验证 DrawObject3D 法线贴图链路：OBJ 推导 tangent → 采样法线贴图 → TBN 变换
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
# Gold Image — PBR 综合全通道材质渲染验证
# （baseColor/metallic/roughness/emissive/AO/normal 六通道同时启用 + TBN）
# ============================================================================
cc_binary(
    name = "jpov_pbr_full_gold_generator",
    srcs = ["jpov_pbr_full_gold_generator.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_metallic_256x256.png",
        ":pbr_roughness_256x256.png",
        ":pbr_emissive_256x256.png",
        ":pbr_ao_256x256.png",
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
    name = "jpov_pbr_full_gold_test",
    srcs = ["jpov_pbr_full_gold_test.cc"],
    data = [
        ":cube_hand.obj",
        ":cube_tex_256x256.png",
        ":pbr_metallic_256x256.png",
        ":pbr_roughness_256x256.png",
        ":pbr_emissive_256x256.png",
        ":pbr_ao_256x256.png",
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



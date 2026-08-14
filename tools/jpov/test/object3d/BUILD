load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

# ============================================================================
# Gold Image — Object3D 无光照（beetle 纯色）渲染验证
# ============================================================================
cc_binary(
    name = "jpov_object3d_gold_generator",
    srcs = ["jpov_object3d_gold_generator.cc"],
    data = [":beetle.obj"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
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
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# Gold Image — PBR Metallic+Roughness 纹理映射验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_cube_normal_gold_generator",
    srcs = ["jpov_pbr_cube_normal_gold_generator.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_gold_test",
    srcs = ["jpov_pbr_cube_normal_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# Gold Image — PBR Cube Normal + Metallic+Roughness 纹理验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_cube_normal_mr_gold_generator",
    srcs = ["jpov_pbr_cube_normal_mr_gold_generator.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_mr_gold_test",
    srcs = ["jpov_pbr_cube_normal_mr_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# Gold Image — PBR Cube Normal + Metallic+Roughness + Emissive+AO 全纹理验证
# ============================================================================
cc_binary(
    name = "jpov_pbr_cube_normal_mr_ea_gold_generator",
    srcs = ["jpov_pbr_cube_normal_mr_ea_gold_generator.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png", ":pbr_emissive_256x256.png", ":pbr_ao_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_mr_ea_gold_test",
    srcs = ["jpov_pbr_cube_normal_mr_ea_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png", ":pbr_emissive_256x256.png", ":pbr_ao_256x256.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

filegroup(
    name = "pliers_gltf_data",
    srcs = glob(["pliers_gltf/*"]),
    visibility = ["//tools/jpov:__subpackages__"],
)

# ============================================================================
# Gold Image — glTF Pliers PBR 渲染验证
# ============================================================================
cc_binary(
    name = "jpov_gltf_pliers_gold_generator",
    srcs = ["jpov_gltf_pliers_gold_generator.cc"],
    data = [":pliers_gltf_data"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_gltf_pliers_gold_test",
    srcs = ["jpov_gltf_pliers_gold_test.cc"],
    data = [":pliers_gltf_data"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

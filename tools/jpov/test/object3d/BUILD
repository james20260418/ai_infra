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

# ============================================================================
# 第三方 glTF/GLB 资源（场景 gold test 用）
# ============================================================================
filegroup(
    name = "scene_assets",
    srcs = glob(["scene_assets/**"]),
    visibility = ["//visibility:public"],
)

# 第三方 glTF/GLB 加载 smoke test（纯 CPU 解析，不渲染）
cc_test(
    name = "jpov_scene_assets_load_test",
    srcs = ["jpov_scene_assets_load_test.cc"],
    data = [":scene_assets"],
    deps = [
        "//tools/jpov/src:gltf_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# 场景 gold test（点光源版）：第三方 glTF + 程序化地面 + 桌/凳/盆栽/墙
# generator 写仓库 gold image (scene_1280x720.png)，test 渲染 + smoke check。
# 共用 jpov_scene_common.h 保证场景一致。

cc_binary(
    name = "jpov_scene_gold_generator",
    srcs = [
        "jpov_scene_gold_generator.cc",
        "jpov_scene_common.h",
    ],
    data = [":scene_assets"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_scene_gold_test",
    srcs = [
        "jpov_scene_gold_test.cc",
        "jpov_scene_common.h",
    ],
    data = [
        ":scene_assets",
        ":scene_1280x720.png",
    ],
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



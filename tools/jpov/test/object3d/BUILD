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

# 场景 gold test（阳光版）：同一布景，光照改用太阳平行光 direction=(0,-1,-1)
# + 级联阴影（CSM）。从点光源版复制，仅光照不同（去掉 3 点光源 → cmds->sun）。
# generator 写仓库 gold image (scene_in_sun_1280x720.png)。

cc_binary(
    name = "jpov_scene_in_sun_gold_generator",
    srcs = [
        "jpov_scene_in_sun_gold_generator.cc",
        "jpov_scene_in_sun_common.h",
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
    name = "jpov_scene_in_sun_gold_test",
    srcs = [
        "jpov_scene_in_sun_gold_test.cc",
        "jpov_scene_in_sun_common.h",
    ],
    data = [
        ":scene_assets",
        ":scene_in_sun_1280x720.png",
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

# box gold test（MeshData::MakeBox + PBRMaterial 纯色构造验证）
# 共享 jpov_box_gold_common.h 的场景构建，纯程序化 mesh（无 glTF 资源）。
# generator 写仓库 gold image (box_1280x720.png)。

cc_binary(
    name = "jpov_box_gold_generator",
    srcs = [
        "jpov_box_gold_generator.cc",
        "jpov_box_gold_common.h",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_box_gold_test",
    srcs = [
        "jpov_box_gold_test.cc",
        "jpov_box_gold_common.h",
    ],
    data = [
        ":box_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)
# 太阳影子（最简版）gold test（DirectionalLight + 正交 shadow map 最小实现）
# 场景：平板 + 立柱，sun direction=(-1,-1,1)，验证立柱在平板上投影子。

cc_binary(
    name = "jpov_sun_shadow_gold_generator",
    srcs = [
        "jpov_sun_shadow_gold_generator.cc",
        "jpov_sun_shadow_common.h",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_sun_shadow_gold_test",
    srcs = [
        "jpov_sun_shadow_gold_test.cc",
        "jpov_sun_shadow_common.h",
    ],
    data = [
        ":sun_shadow_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

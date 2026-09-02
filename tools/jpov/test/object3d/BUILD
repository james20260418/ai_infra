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
        "//tools/jpov/test:test_utils",
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
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_gold_test",
    srcs = ["jpov_pbr_cube_normal_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png", ":pbr_cube_normal_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_mr_gold_test",
    srcs = ["jpov_pbr_cube_normal_mr_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png", ":pbr_cube_normal_mr_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pbr_cube_normal_mr_ea_gold_test",
    srcs = ["jpov_pbr_cube_normal_mr_ea_gold_test.cc"],
    data = [":cube_hand.obj", ":cube_tex_256x256.png", ":pbr_normal_256x256.png", ":pbr_metallic_256x256.png", ":pbr_roughness_256x256.png", ":pbr_emissive_256x256.png", ":pbr_ao_256x256.png", ":pbr_cube_normal_mr_ea_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:obj_loader",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
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

# 内嵌 bufferView 贴图 GLB（Tripo 生成投石车，单文件自包含）
# 用于验证 loader 支持内嵌贴图（无外部 uri）。
filegroup(
    name = "catapult_glb_data",
    srcs = glob(["catapult_glb/*"]),
    visibility = ["//tools/jpov:__subpackages__"],
)

# 内嵌贴图 GLB（Khronos 官方 Lantern，单文件，PBR 全通道：baseColor/normal/
# metallicRoughness(ORM)/emissive，无独立 occlusion）。用于验证内嵌贴图 GLB
# 的完整加载 + AO/emissive 规范语义修复。
filegroup(
    name = "lantern_glb_data",
    srcs = glob(["lantern_glb/*"]),
    visibility = ["//tools/jpov:__subpackages__"],
)

# ============================================================================
# Gold Image — glTF Lantern (内嵌贴图 GLB) PBR 渲染验证
# ============================================================================
cc_binary(
    name = "jpov_gltf_lantern_gold_generator",
    srcs = ["jpov_gltf_lantern_gold_generator.cc"],
    data = [":lantern_glb_data"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_gltf_lantern_gold_test",
    srcs = ["jpov_gltf_lantern_gold_test.cc"],
    data = [":lantern_glb_data", ":lantern_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
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
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_gltf_pliers_gold_test",
    srcs = ["jpov_gltf_pliers_gold_test.cc"],
    data = [":pliers_gltf_data", ":pliers_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
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
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
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
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
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
        "//tools/jpov/test:compare_light",
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
        "//tools/jpov/test:test_utils",
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
        "//tools/jpov/test:compare_light",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)


# ============================================================================
# Gold Image Test — 平放 quad + pbr_cube_normal_mr 金属材质（法线凸起观察）
# 手写平放 quad + 树皮/石头法线/MR 4 贴图 + 三光源，复刻 pbr cube 材质。
# generator 写 gold (repeated_mrquad_1280x720.png)，test 渲染 + smoke check。
# ============================================================================
cc_binary(
    name = "jpov_repeated_mrquad_gold_generator",
    srcs = ["jpov_repeated_mrquad_gold_generator.cc"],
    data = [
        ":brick_seamless_512x512.png",
        ":brick_normal_512x512.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_repeated_mrquad_gold_test",
    srcs = ["jpov_repeated_mrquad_gold_test.cc"],
    data = [
        ":brick_seamless_512x512.png",
        ":brick_normal_512x512.png",
        ":repeated_mrquad_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:test_utils",
        "//tools/jpov/test:compare_light",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ============================================================================
# Gold Image + Test — GPU color-ID 拾取 + 方法 B 高亮边框
# ============================================================================
cc_binary(
    name = "jpov_pick_highlight_gold_generator",
    srcs = [
        "jpov_pick_highlight_gold_generator.cc",
        "jpov_pick_highlight_gold_common.h",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_pick_highlight_gold_test",
    srcs = [
        "jpov_pick_highlight_gold_test.cc",
        "jpov_pick_highlight_gold_common.h",
    ],
    data = [":pick_highlight_1280x720.png"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
        "//third_party/stb:stb_image",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

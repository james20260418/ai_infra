# JPOV Skydome gold test —— 程序化天光（白天/晚霞/夜间 × 双机位）
# 复用 object3d/scene_assets（glTF 家具）作为场景，验证 Preetham 天光。
# generator 写 6 张 gold：skydome_<time>_<cam>.png

cc_binary(
    name = "jpov_skydome_gold_generator",
    srcs = [
        "jpov_skydome_gold_generator.cc",
        "jpov_skydome_common.h",
    ],
    data = ["//tools/jpov/test/object3d:scene_assets"],
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
    name = "jpov_skydome_gold_test",
    srcs = [
        "jpov_skydome_gold_test.cc",
        "jpov_skydome_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:scene_assets",
        ":skydome_day_from_body.png",
        ":skydome_day_to_body.png",
        ":skydome_sunset_from_body.png",
        ":skydome_sunset_to_body.png",
        ":skydome_night_from_body.png",
        ":skydome_night_to_body.png",
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

# ==================== 标准晴天（standard_sunny_day） ====================
# 以 scene_in_sun 为模板 + Preetham 天光 + 太阳平行光对齐，调试正午晴天配色。

cc_binary(
    name = "jpov_standard_sunny_day_gold_generator",
    srcs = [
        "jpov_standard_sunny_day_gold_generator.cc",
        "jpov_standard_sunny_day_common.h",
    ],
    data = ["//tools/jpov/test/object3d:scene_assets"],
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
    name = "jpov_standard_sunny_day_gold_test",
    srcs = [
        "jpov_standard_sunny_day_gold_test.cc",
        "jpov_standard_sunny_day_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:scene_assets",
        ":standard_sunny_day_1280x720.png",
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

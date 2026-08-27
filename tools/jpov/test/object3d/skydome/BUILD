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
        "//tools/jpov/test:compare_light",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# ==================== 太阳轨迹（sun_path） ====================
# 太阳从早到晚的全部天光时段：SkyDirectionAmbient 配置序列（天光 + 太阳方向光
# + 环境光），遍历生成 sun_path_<index>.png。相机 + 场景复用 standard_sunny_day。

cc_binary(
    name = "jpov_sun_path_gold_generator",
    srcs = [
        "jpov_sun_path_gold_generator.cc",
        "jpov_sun_path_common.h",
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
    name = "jpov_sun_path_gold_test",
    srcs = [
        "jpov_sun_path_gold_test.cc",
        "jpov_sun_path_common.h",
        "jpov_standard_sunny_day_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:scene_assets",
        ":sun_path_0.png",
        ":sun_path_1.png",
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

# ==================== picking + highlight（standard_sunny_day 基底） ====================
# 独立 test：以 standard_sunny_day 场景为底座，验证 GPU color-ID 拾取 + 方法 B 高亮。

cc_binary(
    name = "jpov_pick_highlight_sunny_gold_generator",
    srcs = [
        "jpov_pick_highlight_sunny_gold_generator.cc",
        "jpov_pick_highlight_sunny_common.h",
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
    name = "jpov_pick_highlight_sunny_gold_test",
    srcs = [
        "jpov_pick_highlight_sunny_gold_test.cc",
        "jpov_pick_highlight_sunny_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:scene_assets",
        ":pick_highlight_sunny_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/common:utils",
        "//third_party/stb:stb_image",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)


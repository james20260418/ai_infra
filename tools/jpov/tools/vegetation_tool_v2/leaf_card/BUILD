# tools/jpov/tools/vegetation_tool_v2/leaf_card —— 叶卡素材规整（生图 → 合规素材）
#
# 生图模型不知道我们的格子在哪，所以出图之后必须过一道归一化：
#   按生图合同切出内容 → 找叶柄（最细的一端）→ 旋转到 anchor 朝向 → AABB 裁剪。
# 合同（背景/内容亮度阈值）唯一定义在 leaf_card_spec.h。

package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "leaf_card_normalize",
    srcs = ["leaf_card_normalize.cc"],
    hdrs = [
        "leaf_card_normalize.h",
        "leaf_card_spec.h",
    ],
    copts = ["-std=c++20"],
)

# CLI：输入生图 → 输出合规素材 + 诊断（叶柄角度 / 旋转 / 根点偏差 / 宽高比）。
cc_binary(
    name = "jpov_leaf_card",
    srcs = ["jpov_leaf_card.cc"],
    deps = [
        ":leaf_card_normalize",
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "leaf_card_normalize_test",
    srcs = ["leaf_card_normalize_test.cc"],
    deps = [
        ":leaf_card_normalize",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

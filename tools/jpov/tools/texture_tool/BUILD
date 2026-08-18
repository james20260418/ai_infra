# ============================================================================
# JPOV 纹理离线预处理工具
#
# 两个命令行工具（CLI，纯 CPU，无 GL 依赖）：
#   1. normal_map — 从原始图生成法线贴图（sobel 高度场 → 法线，可调强度）
#   2. seamless   — 把输入图周期性化（边缘混合 → tileable，可 GL_REPEAT 平铺）
#
# 依赖：stb_image（读）+ stb_image_write（写 PNG），无第三方图像库。
# 平台：Linux（tools/jpov 的离线工具均以 Linux 为准，供 AI 测图用）。
# ============================================================================

load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "jpov_texture_normal",
    srcs = ["normal_map.cc"],
    deps = [
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_texture_seamless",
    srcs = ["seamless.cc"],
    deps = [
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

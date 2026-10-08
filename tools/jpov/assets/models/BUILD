# tools/jpov/assets/models — JPOV 3D 模型资产库（单一受管路径）
#
# 所有 3D 模型资产（obj / fbx / glb / gltf，含其 .bin 与纹理）都集中在这里，
# 与「gold 图片基线」（仍在 tools/jpov/test/ 下）分离，便于统一管理。
#
# 目录按「用途」而非「来源」分层：
#   primitives/  程序化 / 手写小模型（loader 单测夹具）
#   samples/     单件样例资产（渲染 gold 用）
#   characters/  带骨骼人形 + 动作（Mixamo 系）
#   clothing/    穿衣工具的衣物资产库
#   scene/       第三方场景资产
#
# 路径解析：测试/工具经 jpov::GetModelsDir()（tools/jpov/test/test_utils.h）
# 取前缀；分发态（exe 旁）资源由 build 脚本按需拷贝。
#
# filegroup 是 Bazel 内置 native 规则，无需从 @rules_cc//cc:defs 加载。
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//tools/jpov:__subpackages__"])

# 模型资产路径常量的单一来源（供 demo/tool 以仓库相对路径定位模型）。
cc_library(
    name = "models_path",
    hdrs = ["models_path.h"],
    visibility = ["//visibility:public"],
)

# 程序化 / 手写小模型（OBJ loader 单测夹具；精确数值验证用）。
filegroup(
    name = "primitives",
    srcs = glob(["primitives/*"]),
)

# 内嵌 bufferView 贴图 GLB（Tripo 生成投石车，单文件自包含）
# 用于验证 loader 支持内嵌贴图（无外部 uri）。
filegroup(
    name = "catapult_glb_data",
    srcs = glob(["samples/catapult/*"]),
)

# 内嵌贴图 GLB（Khronos 官方 Lantern，单文件，PBR 全通道：baseColor/normal/
# metallicRoughness(ORM)/emissive，无独立 occlusion）。用于验证内嵌贴图 GLB
# 的完整加载 + AO/emissive 规范语义修复。
filegroup(
    name = "lantern_glb_data",
    srcs = glob(["samples/lantern/*"]),
)

# 薄纱蕾丝裙（Tripo 单图生成 → 蓝标记抠 alpha 后）——透明 / cutout 渲染样例。
#   lace_skirt.glb        : 原始 Tripo PBR 模型（不透明，baseColor=JPEG 无 alpha）。
#   lace_skirt_cutout.glb : 单文件嵌入式 GLB，alphaMode=MASK + alphaCutoff + doubleSided；
#                            baseColor 为内嵌 RGBA PNG（蓝区→薄纱网 alpha）。
# 用途：验证 object3d cutout（alpha test）+ 双面渲染 + **嵌入式** GLB（含 RGBA 贴图）。
filegroup(
    name = "lace_skirt_data",
    srcs = glob(["samples/lace_skirt/*"]),
)

# 外部 uri 的 glTF + bin + jpg（PBR 三贴图，非自包含容器）。
filegroup(
    name = "pliers_gltf_data",
    srcs = glob(["samples/pliers/*"]),
)

# ⚠️ 反面教材（不是可用资产）——Tripo 单图重建复杂植物不可用的证据。
# 2026-09-18 实验：同一张橡树图分别以 P1 低模 3859 面 / 19436 面出模，
# 树冠均为「离散面片堆叠」，未形成连续叶面（拉满面数只救回枝干）。
# 结论：植物类资产应走「纸片 + 透明纹理」，不要用 AI 单图重建。
# 用途：留证 + 决策参照。**故意不挂任何 gold test**——拿已知畸形的模型
# 做渲染回归基准没有意义。详见 samples/oak_negative/README.md。
filegroup(
    name = "oak_tripo_negative_data",
    srcs = glob(["samples/oak_negative/*"]),
)

# wallet_tripo_multiview：多视图（--images）模式的**正面样例**。
# 4 张真正交视图 → 硬表面单体可用模型（结构完整、无破损）。
# 与 oak_negative 对照，可看出「输入结构复杂度」如何决定成败。
filegroup(
    name = "wallet_tripo_multiview_data",
    srcs = glob(["samples/wallet/*"]),
)

# 带骨骼(Tripo Auto Rig spec=mixamo)人形 GLB——gen3d/skeleton 真调资材。
# mixamorig 23 关节 + JOINTS_0/WEIGHTS_0 蒙皮权重; 无预烘焙动画帧。
# 用途: loader 接 skins 的权威资材 + SkeletonType::Mixamo 模板 dump 基准。
filegroup(
    name = "mixamo_male_glb_data",
    srcs = ["characters/mixamo_male.glb"],
)

# Mixamo "Hip Hop Dancing" 全身动画 FBX（ufbx 读入 / 骨架动画 / 重定向资材）。
# 来源: 下载自 mixamo.com 角色动画库；含 1 段舞蹈动画。
filegroup(
    name = "hip_hop_dance_fbx_data",
    srcs = ["characters/hip_hop_dance.fbx"],
)

# clothing：JPOV 穿衣工具（tools/jpov/clothing/）用的衣物资产库。
# 每件衣服一个子目录（glb + 四视图预览 + 生成溯源 README）。
# 现状：harness_vest（战术背心，单图重建，双面）/ yoga_pants（瑜伽裤，文本生成，单面）。
# 用途：为穿衣管线提供可复现、可引用的输入。**不承载 gold test**（外部生成资产，
# 非本项目可控的渲染基准）。详见 clothing/README.md。
filegroup(
    name = "clothing_data",
    srcs = glob(["clothing/**"]),
)

# 第三方 glTF/GLB 场景资源（场景 gold test 用）。
filegroup(
    name = "scene_assets",
    srcs = glob(["scene/**"]),
)

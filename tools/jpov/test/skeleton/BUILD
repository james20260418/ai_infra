# tools/jpov/test/skeleton — 骨架蒙皮渲染 gold 测试(M1:单 pose T-rest 静态)
#
# 独立目录：单独 shader(蒙皮)路径。generator/test 共用 jpov_skeleton_gold_common.h 场景
# （地面 + sunny-day + 左蒙皮人/右 object3d 人）。
# generator(cc_binary) 写仓库 gold PNG；test(cc_test) 渲一帧与 gold 逐像素 diff。
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_skeleton_gold_generator",
    srcs = [
        "jpov_skeleton_gold_generator.cc",
        "jpov_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        "//tools/jpov/test/object3d:scene_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src/skeleton:skeleton",
        "//geom/math:dual_quat",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_skeleton_gold_test",
    srcs = [
        "jpov_skeleton_gold_test.cc",
        "jpov_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        "//tools/jpov/test/object3d:scene_assets",
        ":human_skeleton_gold_t_rest_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src/skeleton:skeleton",
        "//geom/math:dual_quat",
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
# 蒙皮多帧一致性 —— GPU 蒙皮必须与「CPU 真值蒙皮」逐像素同形（25 帧，覆盖 atlas 多行）
#
# 背景：既有 gold 只渲**单个 identity pose**，而 bind 姿态下肤矩阵恒为 I ⇒ 对 atlas 取址
# **完全免疫**（取哪一格都是单位阵，画面一样）。历史上真出过两类 atlas/蒙皮缺陷都躲在
# 这个盲区里：① 烘焙与 shader 对 atlas 2D 布局理解不一致；② 蒙皮用的顶点与骨架不在同一
# 坐标系（2026-09-16 已随「loader 不再改方向」修掉，见 PR #103）。
# 本测试对每一帧同时渲「GPU 蒙皮」与「CPU 按同公式算好的真值蒙皮」，比对左半屏人像的
# 几何掩码与像素差 ⇒ 上面两类缺陷都会当场失败（pose 0 必然 0 差异，其余帧暴露问题）。
# ============================================================================

cc_test(
    name = "jpov_skinned_multipose_test",
    srcs = [
        "jpov_skinned_multipose_test.cc",
        "jpov_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        "//tools/jpov/test/object3d:scene_assets",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src/skeleton:skeleton",
        "//geom/math:dual_quat",
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
# 火柴人（bone mesh）gold —— 标准晴天布景 + identity 位姿红火柴人
#
# 验证 BuildBoneMeshInBoneSpace 生成的骨架空间火柴人能在标准世界里正确渲染
# （docs/jpov_retarget_design.md §4）。generator 写仓库 gold PNG；test 渲染 +
# 可见性门禁（火柴人 ROI 内强红像素计数）+ ROI 平均色对比。
# ============================================================================

cc_binary(
    name = "jpov_bone_mesh_gold_generator",
    srcs = [
        "jpov_bone_mesh_gold_generator.cc",
        "jpov_bone_mesh_gold_common.h",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mixamo23_skeleton",
        "//tools/jpov/interface:skeleton_mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_bone_mesh_gold_test",
    srcs = [
        "jpov_bone_mesh_gold_test.cc",
        "jpov_bone_mesh_gold_common.h",
    ],
    data = [
        ":bone_mesh_stickman_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mixamo23_skeleton",
        "//tools/jpov/interface:skeleton_mesh",
        "//tools/jpov/interface:skeleton_types",
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
# 资产骨架 gold —— skeleton from glb + skeleton from fbx
#
# 覆盖两个读取链路（LoadGltfSkeleton / LoadFbxSkeleton）：把"从资产读出的"两根骨人
# 按无旋转/无缩放/无位移画进同一张图（glb 红 / fbx 蓝，原点重叠），供肉眼对比。
# generator 写仓库 gold PNG；test 渲染 + 双色可见性门禁 + ROI 平均色对比。
# ============================================================================

cc_binary(
    name = "jpov_asset_skeleton_gold_generator",
    srcs = [
        "jpov_asset_skeleton_gold_generator.cc",
        "jpov_asset_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:skeleton_mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
        "//tools/common:utils",
        "//tools/jpov/test:test_utils",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_asset_skeleton_gold_test",
    srcs = [
        "jpov_asset_skeleton_gold_test.cc",
        "jpov_asset_skeleton_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        ":skeleton_from_glb_fbx_1280x720.png",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:skeleton_mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
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
# DQS 蒙皮物理性质（**纯 CPU**，不需 GL/DISPLAY）—— 肘部弯折的“保体积”量化
#
# multipose 测试证明「GPU 的 DQS 与 CPU 真值同形」（实现对不对）；
# 本测试证明「为什么要换 DQS」：同一混合权重下，DQS 是刚体（保距/保体积），
# LBS 是矩阵平均（90° 对分处压到 cos45°≈0.707 倍）。最小 2 骨骨架 + 手写权重，
# 不依赖资产，毫秒级。
# ============================================================================

cc_test(
    name = "jpov_dqs_skinning_test",
    srcs = [
        "jpov_dqs_skinning_test.cc",
        "jpov_skeleton_gold_common.h",
    ],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_types",
        "//geom/math:dual_quat",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

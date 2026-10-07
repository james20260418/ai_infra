load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ============================================================================
# 模型编辑器（model editor）— 独立包（tools/jpov/model_editor/ + jpov::model_editor）
#
# 需求（2026-10-06 Danis）：从穿衣工具（tools/jpov/clothing/）**复制做减法**——
# 只保留「加载 reference（可空）+ target」+「target 平移/旋转/缩放」+「保存」，
# 去掉软体仿真 / 自动蒙皮 / 人体排斥 / 建图；新增**沿水平面裁剪**能力。
#
#   ① 加载：reference（可空）+ target 两个模型；reference 只显示，target 可编辑。
#   ② 左上角面板：target 平移 / 旋转 / 缩放（步进式）+ 保存按钮。
#   ③ 右侧面板：裁剪——沿 y=y0 删一侧（y<y0 / y>y0），裁剪面用 primitives3d
#      （DrawStrip3D）半透明画出。跨面三角形按插值补边界顶点（见 mesh_clip.h）。
#
# 文件：
#   mesh_clip.h        : 沿 y 平面裁剪三角形（纯函数 / GL-free）
#   model_transform.h  : target 平移 / 旋转 / 缩放（就地改顶点，纯函数）
#   number_input.h     : 数值输入解析（纯函数）
#   model_save.{h,cc}  : 保存 glb（后台线程 + 状态机）
#   model_editor_app.h : 渲染核心 App（场景 + 面板 + 唯一 OneIteration）
#   jpov_model_editor.cc : 主程序（CLI 装配 + 交互/headless 分发）
# ============================================================================

# 网格几何操作（裁剪 + 就地变换）：纯 CPU 纯函数，header-only，无 GL。
cc_library(
    name = "model_mesh_ops",
    hdrs = [
        "mesh_clip.h",
        "model_transform.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 数值填值解析（纯函数；header-only）。
cc_library(
    name = "number_input",
    hdrs = ["number_input.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
)

# 保存编排：路径命名 + 后台线程 + 状态机（GL-free）。
cc_library(
    name = "model_save",
    srcs = ["model_save.cc"],
    hdrs = ["model_save.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/common:utils",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/src:gltf_saver",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "model_editor_app",
    hdrs = ["model_editor_app.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":model_mesh_ops",
        ":model_save",
        ":number_input",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/interface:ui",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src:gltf_saver",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_model_editor",
    srcs = ["jpov_model_editor.cc"],
    deps = [
        ":model_editor_app",
        "//tools/jpov/include/jpov:jpov",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 裁剪纯函数单测（含真资材 yoga_pants 的 CPU 端到端：裁剪 → 保存 → 读回自证）。
cc_test(
    name = "mesh_clip_test",
    srcs = ["mesh_clip_test.cc"],
    data = [
        "//tools/jpov/test/object3d:clothing_data",
    ],
    deps = [
        ":model_mesh_ops",
        "//tools/common:utils",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src:gltf_saver",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 就地变换纯函数单测（平移 / 旋转 / 缩放 / 包围盒）。
cc_test(
    name = "model_transform_test",
    srcs = ["model_transform_test.cc"],
    deps = [
        ":model_mesh_ops",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

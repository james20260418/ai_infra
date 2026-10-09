load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ============================================================================
# 穿衣工具（clothing tool）— 独立包
#
# 需求（2026-09-27 Danis）：仿 soft_mesh_simulator 的查看器，做一个把「人体
# reference + 衣服模型」加载并显示出来的可视化工具；要求**单独文件夹和命名空间**
# （即 tools/jpov/clothing/ + namespace jpov::clothing，与 soft_mesh_simulator 互不牵连）。
#
# 能力演进：
#   - M0（2026-09-27）：加载 + 显示。
#   - 2026-09-29：后台建图（最近邻三角形）+ 左上角 x/y/z 填值粗调。
#   - 2026-09-30：完整变换调节 —— 平移、旋转（度，绕 X/Y/Z 逆时针）、整体缩放；
#     **一律直接改衣服 mesh 顶点**（见 clothing_transform.h）；并加「保存衣服 glb」按钮。
#   - 2026-10-01：**纳入软体仿真器**（重力+弹簧+地面）；右上角动力学面板（g/M/F/k）
#     + 暂停/重置按钮；删除「绝对平移」输入框，变换改为**即时**作用于仿真状态。
#   - 2026-10-02：人体排斥（开关 + buffer + 切向速度保留系数）、关联图连通性修复、
#     Reset 修复。
#   - 2026-10-09：**补洞（推进法）面板**——底部中间「执行补洞」+ 细分/平滑 + 最大洞周长阈值；
#     在仿真关联之前把网格破洞补上（mesh_hole_fill.h，与面板同库）。
#   - 2026-10-09：**3D 画笔（选区）+ 中键纵向 pan**——顶部中间画笔面板（激活 / 半径滑条）；
#     激活态下左键涂抹把邻近顶点选入选区（brush_tool.h / camera_projection.h）；
#     中键纵向 pan 注视点（半屏 = 1m）。
#     仍不做：穿衣对齐 / 自动贴合。
#
#   clothing_tool_app : 渲染核心 App（场景 + 面板宿主 + 唯一 OneIteration）
#   jpov_clothing_tool: 主程序（CLI 装配 + 交互/headless 分发）
# ============================================================================

# 数值填值解析（纯函数；header-only，无 GL/glog 依赖），单列一个库便于纯单测。
cc_library(
    name = "clothing_axis_input",
    hdrs = ["clothing_axis_input.h"],
    visibility = ["//visibility:public"],
    copts = ["-std=c++20"],
)

# 衣物 mesh 就地几何操作（平移/旋转/缩放 + 法线重算；纯函数；无 GL，仅依赖 mesh）。
cc_library(
    name = "clothing_transform",
    hdrs = ["clothing_transform.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 软布自动蒙皮（weight transfer）—— header-only 纯函数，无 GL / glTF / 渲染栈依赖。
# 单独成库：① 供 clothing_tool_app 使用；② 可被纯单测直接依赖（不拖入整个 JPOV 渲染栈）。
cc_library(
    name = "weight_transfer",
    hdrs = ["weight_transfer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/3d:triangle_3",
        "//geom/3d:triangle_matcher_3d",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 衣物网格补洞（推进法 / Liepa 风格）—— header-only 纯函数，无 GL。
# 单独成库：① 供 clothing_tool_app 在仿真前调用（“关联前先补洞”）；② 可纯单测。
cc_library(
    name = "mesh_hole_fill",
    hdrs = ["mesh_hole_fill.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 随机摆动测试姿态发生器（pure header-only；只依赖 geom+skeleton_types，无 GL）。
cc_library(
    name = "random_pose_driver",
    hdrs = ["random_pose_driver.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:quaternion",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 3D 画笔（选区）工具 + 相机投影 —— header-only 纯函数，无 GL。
#   brush_tool.h      选区状态（各 primitive 的顶点 index）+ 射线选点谓词；
#   camera_projection.h  相机基 / 像素↔世界投影（与渲染器 lookAt 约定一致）。
# 单独成库：① 供 clothing_tool_app 使用；② 可纯单测（不拖渲染栈）。
cc_library(
    name = "brush_tool",
    hdrs = [
        "brush_tool.h",
        "camera_projection.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:camera",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "clothing_tool_app",
    srcs = [
        "clothing_init.cc",
        "clothing_save.cc",
    ],
    hdrs = [
        "clothing_init.h",
        "clothing_save.h",
        "clothing_tool_app.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":brush_tool",
        ":clothing_axis_input",
        ":clothing_transform",
        ":mesh_hole_fill",
        ":random_pose_driver",
        ":weight_transfer",
        "//geom/3d:triangle_3",
        "//geom/3d:triangle_matcher_3d",
        "//tools/common:utils",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:view_config",
        "//tools/jpov/assets/models:models_path",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/interface:ui",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/src:gltf_saver",
        "//tools/jpov/soft_mesh_simulator:soft_mesh_simulator",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 坐标填值输入解析（纯函数单测；无 GL）。
cc_test(
    name = "clothing_axis_input_test",
    srcs = ["clothing_axis_input_test.cc"],
    deps = [
        ":clothing_axis_input",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 衣物变换纯函数单测（clamp 语义 / 欧拉角方向 / 烘焙坐标；无 GL）。
cc_test(
    name = "clothing_transform_test",
    srcs = ["clothing_transform_test.cc"],
    deps = [
        ":clothing_transform",
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

# 随机摆动姿态发生器单测（确定性 / 周期性 / 限幅 / 根不动；无 GL）。
cc_test(
    name = "random_pose_driver_test",
    srcs = ["random_pose_driver_test.cc"],
    deps = [
        ":random_pose_driver",
        "//geom/common:quaternion",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 网格补洞纯函数单测（闭合体挖洞→补齐 / 绕序 / 焊接缝合 / 阈值；无 GL）。
cc_test(
    name = "mesh_hole_fill_test",
    srcs = ["mesh_hole_fill_test.cc"],
    deps = [
        ":mesh_hole_fill",
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

# 3D 画笔（选区）工具 + 相机投影 纯函数单测（生命周期 / 半径换算 / 投影往返；无 GL）。
cc_test(
    name = "brush_tool_test",
    srcs = ["brush_tool_test.cc"],
    deps = [
        ":brush_tool",
        "//tools/jpov/interface:camera",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 软布自动蒙皮（weight transfer）纯函数单测（重心插值 / top-K / 种子生长 / 焊接 / 离体兜底；无 GL）。
cc_test(
    name = "weight_transfer_test",
    srcs = ["weight_transfer_test.cc"],
    deps = [
        ":weight_transfer",
        "//geom/3d:triangle_3",
        "//geom/3d:triangle_matcher_3d",
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

# 端到端（CPU-only）：真资材 mixamo_male + harness_vest → 自动蒙皮 → 写带 skin glb
# → 用生产 loader 读回自证（JOINTS/WEIGHTS/骨数/归一）。
cc_test(
    name = "clothing_skin_e2e_test",
    srcs = ["clothing_skin_e2e_test.cc"],
    data = [
        "//tools/jpov/assets/models:clothing_data",
        "//tools/jpov/assets/models:mixamo_male_glb_data",
    ],
    deps = [
        ":weight_transfer",
        "//geom/3d:triangle_3",
        "//geom/3d:triangle_matcher_3d",
        "//tools/common:utils",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_types",
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

cc_binary(
    name = "jpov_clothing_tool",
    srcs = ["jpov_clothing_tool.cc"],
    deps = [
        ":clothing_tool_app",
        "//tools/jpov/include/jpov:jpov",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

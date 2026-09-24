load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# IdAllocator — 可复用/避回绕的 uint32 句柄分配器（freelist / LIFO + live 集）。
# 作为【新增】skeleton renderer 侧 id 管理；将来修 MeshManager/TextureManager 的裸递增回绕
# bug 时同款（见 id_allocator.h 文件头）。
cc_library(
    name = "id_allocator",
    srcs = ["id_allocator.cc"],
    hdrs = ["id_allocator.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

cc_test(
    name = "id_allocator_test",
    srcs = ["id_allocator_test.cc"],
    deps = [
        ":id_allocator",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# InputSnapshot — 帧级输入抽象
cc_library(
    name = "input_snapshot",
    hdrs = ["input_snapshot.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@glog_static//:glog",
    ],
)

# PBRMaterial — 叶子材质类型（被 render_command / gltf_object 共用）
cc_library(
    name = "pbr_material",
    hdrs = ["pbr_material.h"],
    visibility = ["//visibility:public"],
    deps = [],
)

# RenderCommand — 帧级渲染指令输出
cc_library(
    name = "render_command",
    srcs = ["render_command.cc"],
    hdrs = ["render_command.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":camera",
        ":gltf_object",
        ":pbr_material",
        ":skeleton_types",
        "//geom/common:vec",
        "//geom/math:piecewise_linear_function",
        "@glog_static//:glog",
    ],
)

# SkyCommand 平行光推导单测（纯 CPU）：月亮直射光的颜色/强度 + 太阳强度零回归锚点。
cc_test(
    name = "sky_lighting_test",
    srcs = ["sky_lighting_test.cc"],
    deps = [
        ":render_command",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SkeletonTypes — 骨架蒙皮 CPU 侧数据定义（SkeletonType/SkeletonPose/SkinnedInstanceState）
# 被 render_command（SkinnedMeshCommand / DrawMeshWithSkeleton 引用）与 src/skeleton（GPU 管理器）共用。
# 头里用到 geom::Quaternion（SkeletonPose.joint_rotation）、geom::math::Mat4（ComputeInverseBind）
# 与 Vec，故须公开依赖 quaternion / vec / mat4。
cc_library(
    name = "skeleton_types",
    hdrs = ["skeleton_types.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:quaternion",
        "//geom/common:vec",
        "//geom/math:mat4",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# Mixamo23Skeleton — Mixamo 官方 23 骨骨架工厂（程序化生成，无资产）。
# 数据从官方 FBX 实测反推（见 header 头注）。纯 CPU/GL-free。
cc_library(
    name = "mixamo23_skeleton",
    hdrs = ["mixamo23_skeleton.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "mixamo23_skeleton_test",
    srcs = ["mixamo23_skeleton_test.cc"],
    deps = [
        ":mixamo23_skeleton",
        ":skeleton_types",
        "//geom/common:math_util",
        "//geom/math:mat4",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# SkeletonRetarget — 骨架动作重定向（Q 桥接量）：源 pose → 目标 pose（CPU、GL-free）
# 从两个 SkeletonType 建 Q（骨名对齐 + Q 表），再逐帧把源 pose 搬到目标骨架。
# 见 docs/jpov_retarget_design.md §1.2 / §6 / §7-M4。
cc_library(
    name = "skeleton_retarget",
    hdrs = ["skeleton_retarget.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "skeleton_retarget_test",
    srcs = ["skeleton_retarget_test.cc"],
    deps = [
        ":mixamo23_skeleton",
        ":skeleton_retarget",
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# SkeletonMesh — 骨架可视化（火柴人 mesh）CPU 几何生成（GL-free）
# 由 SkeletonType + Pose 造出可直接 RegisterMesh 的带骨杆 mesh（见 docs/jpov_retarget_design.md §4）。
cc_library(
    name = "skeleton_mesh",
    hdrs = ["skeleton_mesh.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mesh",
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "//geom/math:mat4",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# AxisGizmo — 世界坐标轴辅助几何（1m X/Y/Z 三色条带，宽 5cm，alpha 0.2）。
# 纯几何构造（3D 条带顶点，零 GL、零编辑器状态），与 skeleton_mesh 同层：
# 任何工具想做"画个坐标架"都能复用，不绑在某个 demo 上。
#
# ⚠️ 用 **3D 条带（DrawStrip3D）** 而非 Object3D 网格：条带的 FS 是
#    `FragColor = uColor`，alpha 天然生效 —— 这是刻意选择，为的是
#    **完全不动 Object3D 的着色通路**（Danis 定调 2026-09-14：减少影响面）。
# 依赖 render_command 是为了用 Strip3DCommand 的顶点类型（Vec3f）。
cc_library(
    name = "axis_gizmo",
    hdrs = ["axis_gizmo.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":pbr_material",
        ":render_command",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "axis_gizmo_test",
    srcs = ["axis_gizmo_test.cc"],
    deps = [
        ":axis_gizmo",
        ":pbr_material",
        ":render_command",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

cc_test(
    name = "skeleton_mesh_test",
    srcs = ["skeleton_mesh_test.cc"],
    # 真实资产回归：mixamo_male.glb（顶层 Root 是 4.5mm 导出残差 → 讨要"根杆收窄"）。
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
    ],
    deps = [
        ":mixamo23_skeleton",
        ":skeleton_mesh",
        ":skeleton_types",
        "//geom/math:mat4",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# MeshTransform — 把绘制时的放置参数 (center/up/front/scale) 烘进 CPU 资产（MeshData /
# SkeletonType）。GL-free 纯函数层；模型编辑器保存 glb 时用它把"摆好的姿态"固化进几何与骨架
# （含"顶点烘 U ⟺ 骨架共轭 U"的一致性，见 mesh_transform.h 文件头）。
cc_library(
    name = "mesh_transform",
    hdrs = ["mesh_transform.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mesh",
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "mesh_transform_test",
    srcs = ["mesh_transform_test.cc"],
    deps = [
        ":mesh",
        ":mesh_transform",
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/math:mat4",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# AnimationClip — CPU 侧外部动画素材容器（FBXClip：源骨架 + 帧频 + 原始全帧）
# 承载 src/fbx_loader 的产出；供上层 debug/retarget/resample 二次开发用，不碰 GPU。
# （src/skeleton 的 GPU pose atlas 使用另见 src/skeleton。）
cc_library(
    name = "animation_clip",
    hdrs = ["animation_clip.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":skeleton_types",
    ],
    copts = ["-std=c++20"],
)

# AnimationSampler — 动画素材的【时间采样】（某时刻 → 一个 SkeletonPose）
# FBXClip 的取帧原语：帧网格归属 + 逐骨球面插值 + 循环回绕。供 FBX 观察器逐帧取姿、
# 后续 retarget 按时间取源姿态同款（纯 CPU / GL-free，见 animation_sampler.h 头注）。
cc_library(
    name = "animation_sampler",
    hdrs = ["animation_sampler.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":animation_clip",
        ":skeleton_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "animation_sampler_test",
    srcs = ["animation_sampler_test.cc"],
    deps = [
        ":animation_sampler",
        "//geom/common:quaternion",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Camera — 透视相机配置
cc_library(
    name = "camera",
    hdrs = ["camera.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
    ],
)

# Text3DUtil — 3D 文本的像素/米 换算（纯函数，GL-free，可单测）
# 见 text3d_util.h：PixelsPerMeterAt(anchor, camera, fbo_h)。
cc_library(
    name = "text3d_util",
    hdrs = ["text3d_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":camera",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# WindowInfo — 帧级窗口信息
cc_library(
    name = "window_info",
    hdrs = ["window_info.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@glog_static//:glog",
    ],
)

# MeshData — CPU 侧网格数据定义（顶点属性位掩码 + 逐属性数组 + 校验）
cc_library(
    name = "mesh",
    hdrs = ["mesh.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mesh_geometry",
    hdrs = ["mesh_geometry.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# GPUMesh — CPU MeshData 上传到 GPU 后的句柄（VAO + 分离 VBO + EBO）
cc_library(
    name = "gpumesh",
    hdrs = ["gpumesh.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mesh",
    ],
    copts = ["-std=c++20"],
)

# jpov_interface — 聚合所有接口（方便用户一次性引用）
cc_library(
    name = "ui",
    srcs = ["ui.cc"],
    hdrs = ["ui.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":input_snapshot",
        ":pbr_material",
        ":render_command",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "gltf_object",
    hdrs = ["gltf_object.h"],
    visibility = ["//visibility:public"],
    deps = [":pbr_material"],
)

cc_library(
    name = "jpov_interface",
    hdrs = [
        "camera.h",
        "input_snapshot.h",
        "render_command.h",
        "mesh.h",
        "gpumesh.h",
        "window_info.h",
        "gltf_object.h",
        "pbr_material.h",
        "ui.h",
        "skeleton_types.h",
        "animation_clip.h",
        "animation_sampler.h",
        "text3d_util.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":camera",
        ":gltf_object",
        ":gpumesh",
        ":input_snapshot",
        ":mesh",
        ":pbr_material",
        ":render_command",
        ":window_info",
        ":ui",
        ":skeleton_types",
        ":animation_clip",
        ":animation_sampler",
        ":text3d_util",
    ],
)



cc_test(
    name = "mesh_geometry_test",
    srcs = ["mesh_geometry_test.cc"],
    deps = [
        ":mesh",
        ":mesh_geometry",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

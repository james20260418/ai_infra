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

cc_test(
    name = "skeleton_mesh_test",
    srcs = ["skeleton_mesh_test.cc"],
    deps = [
        ":mixamo23_skeleton",
        ":skeleton_mesh",
        ":skeleton_types",
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

# Camera — 透视相机配置
cc_library(
    name = "camera",
    hdrs = ["camera.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
    ],
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
    ],
)

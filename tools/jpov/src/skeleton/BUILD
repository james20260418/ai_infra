load("@rules_cc//cc:defs.bzl", "cc_library")

# Skeleton subsystem — GPU 侧骨架蒙皮资源（骨骼动画纹理烘焙 + instancing 蒙皮绘制）。
# CPU 侧纯类型在 //tools/jpov/interface:skeleton_types；此处是 src/ 的 GL 管理器实现
# （skeleton_manager.cc：pose atlas 烘焙/上传 + GpuHandles 句柄出口，方案甲折入 inverse_bind）。
#
# 布局对齐其它 src 子渲染器（object3d/primitives3d/…）：gl 代码在 src/<name>/，其被
# renderer 依赖。SkeletonManager 一个实例持一份骨架+骨骼纹理（多骨架=多实例，见头文件）。
# ⚠️ 仅用可控 RGBA32F 纹理（glTexImage2D/glTexSubImage2D），不用 SSBO——跨 MinGW/Linux 不可移植。
cc_library(
    name = "skeleton",
    srcs = ["skeleton_manager.cc"],
    hdrs = [
        "skeleton_manager.h",
        # 蒙皮渲染的 shader / bind-pose cover（与 skeleton_manager 平级，前缀 skinning_）
        "skinning_shader.h",
        "skinning_bind_pose.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:skeleton_types",
        "//geom/common:quaternion",
        "@glog_static//:glog",
    ] + select({
        "@platforms//os:windows": [
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": [],
    }),
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SkeletonRenderer — 蒙皮带骨物体渲染（独立子渲染器，与 Object3DRenderer 在 renderer
# 层面平级、互不依赖）。Phase 1：整份 Object3DRenderer 复制过来先跑通 gold（渲染效果零
# 变化，仅把类名/调用方换成 SkeletonRenderer），后续逐步做减法剥离。
cc_library(
    name = "skeleton_renderer",
    srcs = ["skeleton_renderer.cc"],
    hdrs = ["skeleton_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/src:mesh_manager",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src:texture_manager",
        "//tools/jpov/src/skeleton:skeleton",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:camera",
        "@glog_static//:glog",
    ] + select({
        "@platforms//os:windows": [
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": [],
    }),
    copts = ["-std=c++20"],
    linkstatic = 1,
)

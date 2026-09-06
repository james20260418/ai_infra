load("@rules_cc//cc:defs.bzl", "cc_library")

# Skeleton subsystem — GPU 侧骨架蒙皮资源（骨骼动画纹理烘焙 + instancing 蒙皮绘制）。
# CPU 侧纯类型在 //tools/jpov/interface:skeleton_types；此处是 src/ 的 GL 管理器声明
# （framework 阶段仅头，实现.h 方法后续在同目录 .cc 注入）。
#
# 布局对齐其它 src 子渲染器（object3d/primitives3d/…）：gl 代码在 src/<name>/，其被
# renderer 依赖。SkeletonManager 一个实例持一份骨架（多骨架=多实例，见头文件）。
cc_library(
    name = "skeleton",
    hdrs = ["skeleton_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

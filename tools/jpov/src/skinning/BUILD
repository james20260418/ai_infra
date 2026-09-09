# JPOV skeleton — 蒙皮渲染层（M1）
#
# 本模块是「骨架蒙皮渲染」的 shader / draw 层：蒙皮 VS(kSkinnedVs) + bind pose cover
# (MakeMixamoBindPose)。renderer 依赖本库在 DrawMeshWithSkeleton 里取用。
# 目前仅头文件（VS/cover），draw 实现后续接（见 docs/jpov_skinning_shader_m1_design.md §6）。
# 依赖 interface:skeleton_types（SkeletonType/SkeletonPose）与 geom quaternion。
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "skinning",
    hdrs = [
        "skinned_mesh_renderer.h",
        "mixamo_bind_pose.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:skeleton_types",
        "//geom/common:quaternion",
    ],
    copts = ["-std=c++20"],
)

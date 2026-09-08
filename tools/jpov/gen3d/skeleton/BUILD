# gen3d/skeleton — 带骨骼 3D 模型生成（占位骨架）
#
# 本期 PR 只建结构占位：本项目为空头 + 命名空间约定（jpov::gen3d），真实生成逻辑
# 后续实现（先不动 gltf loader / src 渲染，见 docs/jpov_clothes_rig_design.md）。
# 占位 cc_library 保证骨架 target 存在、可被 bazel 编译，便于 sibling 脚本引用与验证。

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "skeleton_rig",
    hdrs = ["skeleton_rig.h"],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

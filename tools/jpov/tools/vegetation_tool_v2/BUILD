# 植物资产工具 v2 —— 拓扑格式（ShapeProto）+ 运行时结构 + 校验。
#
# 设计见 docs/vegetation_asset_format.md。

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test", "cc_proto_library")
load("@rules_proto//proto:defs.bzl", "proto_library")

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "shape_proto_src",
    srcs = ["proto/shape.proto"],
)

cc_proto_library(
    name = "shape_proto",
    deps = [":shape_proto_src"],
)

# 运行时结构 + 校验。下游只依赖这个 target，不直接依赖 protobuf。
cc_library(
    name = "shape",
    srcs = ["shape.cc"],
    hdrs = ["shape.h"],
    deps = [":shape_proto"],
)

cc_test(
    name = "shape_test",
    srcs = ["shape_test.cc"],
    deps = [":shape"],
)

# 扫管：Shape -> 网格 + UV。
cc_library(
    name = "sweep",
    srcs = ["sweep.cc"],
    hdrs = ["sweep.h"],
    deps = [":shape"],
)

cc_test(
    name = "sweep_test",
    srcs = ["sweep_test.cc"],
    deps = [":sweep"],
)

# 可视化工具：把 ShapeProto（pb.txt）渲染出来看形状。
# 刻意不接材质——这一阶段要看的是拓扑/UV 尺度/分叉，贴图会干扰判断。
cc_binary(
    name = "jpov_vegetation_v2_viewer",
    srcs = ["viewer/vegetation_v2_viewer.cc"],
    deps = [
        ":shape",
        ":sweep",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:ui",
        "@com_google_protobuf//:protobuf",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

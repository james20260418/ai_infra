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

# 植物组合查看器：给我 shape / bark / leaf，我把植物画出来。
#   * shape 省略 → 打包目录里的 shapes/willow.pbtxt
#   * bark / leaf 省略 → 只渲染拓扑（树干按 level 上棕色 + debug 绿卡）
#
# 场景 + 渲染在 cc_library(vegetation_v2_app) 里**只编译一次**（.h 只有接口 + CliArgs）：
# 曾经是 header-only，每个 consumer 各编译一份实现，叶卡 alpha 覆盖率 mip 的浮点细节
# 不同 ⇒ gold 与用户所见稳定差 12%。CLI 壳（vegetation_v2_viewer.cc）只调 RunViewer()。

cc_library(
    name = "vegetation_v2_app",
    srcs = ["viewer/vegetation_v2_app.cc"],
    hdrs = ["viewer/vegetation_v2_app.h"],
    deps = [
        ":shape",
        ":sweep",
        "//tools/jpov:skylight_scene",
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:ui",
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
        "@com_google_protobuf//:protobuf",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

cc_binary(
    name = "jpov_vegetation_v2_viewer",
    srcs = ["viewer/vegetation_v2_viewer.cc"],
    deps = [
        ":vegetation_v2_app",
        "@glog_static//:glog",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# gold test 用的输入：一个范本拓扑 + 一份样例叶卡 + 一份样例树皮。
# 用 filegroup 暴露，让 test/vegetation_v2 可以 data 依赖（samples/ 与 assets/ 不是 bazel 包）。
filegroup(
    name = "sample_assets",
    srcs = [
        "assets/bark/bark001/albedo.png",
        "assets/bark/bark001/normal.png",
        "assets/bark/bark001/roughness.png",
        "assets/leaf/willow_leaf.png",
        "samples/willow.pbtxt",
    ],
)

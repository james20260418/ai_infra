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

# 生成配置（喂给 Blender/Sapling 的那组参数，见 proto/generate.proto）。
#
# 只有 python 工具会读它（generate_shape.py 自己解析文本格式：Blender 自带的 python
# 没有 protobuf runtime，镜像里的系统 python 也没有，和 shape.pbtxt 的选择一致）。
# 放成 proto_library 只是让构建期用 protoc 校验 schema 本身没写错。
proto_library(
    name = "generate_proto_src",
    srcs = ["proto/generate.proto"],
)

# C++ 侧读配置用（shape_gen）：TextFormat 解析 + 字段存在性（proto2 optional）。
cc_proto_library(
    name = "generate_proto",
    deps = [":generate_proto_src"],
)

# 入库的生成配置（输入数据，不是构建产物）。
filegroup(
    name = "generate_configs",
    srcs = glob(["configs/*.pbtxt"]),
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

# 叶卡贴图的 CPU 流水线（黑底→alpha / 法线贴图 / PNG 编解码）：查看器与 GLB 烘焙共用。
cc_library(
    name = "leaf_texture",
    srcs = ["leaf_texture.cc"],
    hdrs = ["leaf_texture.h"],
    deps = [
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
        "@glog_static//:glog",
    ],
)

# 烘焙参数（GlbBakeOptions / GlbBakeStats）：baker 与关节构造共用，避免循环 include。
cc_library(
    name = "bake_options",
    hdrs = ["bake/bake_options.h"],
)

# 植被风关节表构造（GL-free，纯几何）：烘焙与单测共用。
cc_library(
    name = "vegetation_glb_joints",
    srcs = ["bake/vegetation_glb_joints.cc"],
    hdrs = ["bake/vegetation_glb_joints.h"],
    deps = [
        ":bake_options",
        ":shape",
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
)

cc_test(
    name = "vegetation_glb_joints_test",
    srcs = ["bake/vegetation_glb_joints_test.cc"],
    deps = [
        ":bake_options",
        ":vegetation_glb_joints",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# GLB 烘焙器：shape（+bark 材质 + 叶卡素材）→ 自洽 .glb（含风链 TEXCOORD_1 + 关节表）。
cc_library(
    name = "vegetation_glb_baker",
    srcs = ["bake/vegetation_glb_baker.cc"],
    hdrs = ["bake/vegetation_glb_baker.h"],
    deps = [
        ":bake_options",
        ":leaf_texture",
        ":shape",
        ":sweep",
        ":vegetation_glb_joints",
        "//tools/jpov/src:gltf_saver",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
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
        ":leaf_texture",
        "//tools/jpov/tools/vegetation_tool_v2/leaf_card:leaf_card_spec",
        ":shape",
        ":sweep",
        ":vegetation_glb_baker",
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

# 「真树」范本拓扑（2026-10-27 入库）：松 / 橡 / 杉，各 1.8k~4.7k 张叶卡。
#   willow.pbtxt 是 442 卡的小苗，只够验拓扑/叶卡；要看**风力/实例化**这类
#   「树冠越大越明显」的观感必须用这三棵。为什么单开一个 filegroup：
#   ~4 MB，不想让只需要 willow 的 gold test 把它们一起拖进 runfiles。
#   生成方式见 samples/README.md（configs/*.pbtxt → generate_shape.py）。
filegroup(
    name = "sample_trees",
    srcs = [
        "samples/fir_low.pbtxt",
        "samples/oak_low.pbtxt",
        "samples/pine_low.pbtxt",
    ],
)

# 导出链的 Blender 侧脚本：只把 Sapling 的曲线 / 叶卡网格倒成 .dump（几何决策在 gen/）。
exports_files(["dump_sapling_curve.py"])

# gen3d/skeleton — 带骨骼(mixamo rig)3D 模型生成
#
# 与 gen3d/static(静态 PBR)并列。Tripo v3 两步 task 链：
#   text-to-model(静态) → Auto Rig(spec=mixamo) → 下载带骨骼 GLB。
# 独立 RigClient(skeleton_rig.cc, 自实现 curl 基础设施), 不侵入 static 稳定代码。
# namespace 约定: jpov::gen3d(同 static tripo_client)。

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

# 库: 供应商无关 RigConfig + TripoRigClient。
cc_library(
    name = "gen3d_skeleton",
    srcs = ["skeleton_rig.cc"],
    hdrs = [
        "rig_config.h",
        "skeleton_rig.h",
    ],
    deps = [
        "//third_party/curl-static:curl",
        "//third_party/nlohmann-json:json",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# gen3d_skeleton_cmd: text→带骨骼 GLB（Linux ELF）。
# 渲染/动画交给后续 JPOV 渲染侧, 本 cmd 只产出带 mixamorig 骨骼的 .glb。
cc_binary(
    name = "gen3d_skeleton_cmd",
    srcs = ["gen3d_skeleton_cmd.cc"],
    deps = [
        ":gen3d_skeleton",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

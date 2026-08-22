load("@rules_cc//cc:defs.bzl", "cc_library")

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
        "//geom/common:vec",
        "//geom/math:piecewise_linear_function",
        "@glog_static//:glog",
    ],
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
    ],
)

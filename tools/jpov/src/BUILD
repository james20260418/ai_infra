load("@rules_cc//cc:defs.bzl", "cc_library")

# ShaderManager — GLSL shader program 注册/编译/缓存管理
cc_library(
    name = "shader_manager",
    srcs = ["shader_manager.cc"],
    hdrs = ["shader_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
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

# TextureManager — GPU 纹理生命周期管理（PNG 加载/去重/释放）
cc_library(
    name = "texture_manager",
    srcs = ["texture_manager.cc"],
    hdrs = ["texture_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/jpov/interface:id_allocator",
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

# MeshManager — CPU MeshData → GPU（VAO + 分离 VBO + EBO）上传/更新/释放
cc_library(
    name = "mesh_manager",
    srcs = ["mesh_manager.cc"],
    hdrs = ["mesh_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:gpumesh",
        "//tools/jpov/interface:id_allocator",
        "//tools/jpov/interface:mesh",
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

# TextureUnits — 全渲染器唯一的纹理单元分配表（header-only）。
# 所有 glActiveTexture 引用此表，禁止裸数字；区间重叠由文件内 static_assert 编译期拦截。
cc_library(
    name = "texture_units",
    hdrs = ["texture_units.h"],
    visibility = ["//visibility:public"],
)

# InstanceBuffer — 一次 Instanced draw 的 per-instance attribute 数据缓冲。
# 实例数据属于「这次 draw」（每帧由调用方给），不属于不可变几何（GPUMesh），
# 故由渲染器持有、跨 draw 复用，draw 前临时挂到 mesh VAO（RAII 配对，见头文件）。
cc_library(
    name = "instance_buffer",
    srcs = ["instance_buffer.cc"],
    hdrs = ["instance_buffer.h"],
    visibility = ["//visibility:public"],
    deps = [
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

# OBJLoader — Wavefront .obj 文本 → CPU MeshData 解析
cc_library(
    name = "obj_loader",
    srcs = ["obj_loader.cc"],
    hdrs = ["obj_loader.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:mesh",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# GltfLoader — glTF 2.0 → MeshData + 材质贴图
cc_library(
    name = "gltf_loader",
    srcs = ["gltf_loader.cc"],
    hdrs = ["gltf_loader.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:pbr_material",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/src:orm_unpack",
        "//third_party/tinygltf",
        "//third_party/stb:stb_image",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# FbxLoader — FBX 骨骼动画源 → FBXClip（CPU 加载, 不碰 GL）
cc_library(
    name = "fbx_loader",
    srcs = ["fbx_loader.cc"],
    hdrs = ["fbx_loader.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:animation_clip",
        "//third_party/ufbx-static:ufbx",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# OrmUnpack — ORM 三通道拆包为独立灰度 PNG
cc_library(
    name = "orm_unpack",
    srcs = ["orm_unpack.cc"],
    hdrs = ["orm_unpack.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//third_party/stb:stb_image_write",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20", "-DSTB_IMAGE_WRITE_IMPLEMENTATION"],
    linkstatic = 1,
)

# FontManager — 字体加载 / 光栅化 / 图集管理 / 顶点生成
cc_library(
    name = "font_manager",
    srcs = ["font_manager.cc"],
    hdrs = ["font_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:render_command",
        "@glog_static//:glog",
        "//third_party/stb:stb_truetype",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# SkyRenderer — 程序化天光背景（独立轻量 shader，一帧一次）
cc_library(
    name = "sky_renderer",
    srcs = ["skydome/sky_renderer.cc"],
    hdrs = ["skydome/sky_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":shader_manager",
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

# HorizonFogRenderer — 远景仰角雾（空气透视）：一次全屏后处理（独立子渲染器）。
# 不持有自己的 FBO：用 alpha 混合就地合成到调用方当前绑定的 3D HDR FBO。
cc_library(
    name = "horizon_fog_renderer",
    srcs = ["horizon_fog/horizon_fog_renderer.cc"],
    hdrs = [
        "horizon_fog/horizon_fog_renderer.h",
        # 雾的着色器（与 renderer 平级，前缀 horizon_fog_）
        "horizon_fog/horizon_fog_shader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":shader_manager",
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

# GltfSaver — CPU 资产（MeshData + 材质 + 可选骨架）→ 自洽 .glb（GL-free）
cc_library(
    name = "gltf_saver",
    srcs = ["gltf_saver.cc"],
    hdrs = ["gltf_saver.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":gltf_loader",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_types",
        "//third_party/nlohmann-json:json",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# Renderer — GL FBO + 动态分辨率渲染核心
cc_library(
    name = "renderer",
    srcs = ["renderer.cc"],
    hdrs = ["renderer.h"],
    data = ["//tools/jpov/fonts:all_fonts"],
    visibility = ["//visibility:public"],
    deps = [
        ":font_renderer",
        ":primitives2d_renderer",
        ":mesh_manager",
        ":object3d_renderer",
        ":instanced_renderer",
        ":primitives3d_renderer",
        "//tools/jpov/src/skeleton:skeleton_renderer",
        ":shader_manager",
        ":sky_renderer",
        ":horizon_fog_renderer",
        "//tools/jpov/src/fire_fog:fire_fog_renderer",
        ":texture_manager",
        ":gltf_loader",
        ":orm_unpack",
        "//tools/common:utils",
        "//tools/jpov/src/skeleton:skeleton",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "//tools/jpov/interface:gltf_object",
        "//tools/jpov/interface:text3d_util",
        "//geom/common:common",
        "@glog_static//:glog",
        "//third_party/stb:stb_image",
        "//third_party/stb:stb_image_write",
    ] + select({
        "@platforms//os:windows": [
            "@glfw_mingw//:glfw",
            "//third_party/gl_loader-mingw:gl_loader",
        ],
        "//conditions:default": ["@glfw_static//:glfw"],
    }),
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# FontRenderer — 字体 2D 渲染（注册/atlas/DrawText2D）
cc_library(
    name = "font_renderer",
    srcs = ["font2d/font_renderer.cc"],
    hdrs = ["font2d/font_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":font_manager",
        ":primitives3d_renderer",
        "//tools/jpov/interface:render_command",
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

# Object3DRenderer — PBR DrawObject3D + Tile Forward 光照
cc_library(
    name = "object3d_renderer",
    srcs = ["object3d/object3d_renderer.cc"],
    hdrs = ["object3d/object3d_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mesh_manager",
        ":shader_manager",
        ":texture_manager",
        ":texture_units",
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

# InstancedObjectRenderer — 静态模型批量实例渲染（DrawInstancedObject）+ 实例阴影。
# 独立子渲染器，与 Object3DRenderer / SkeletonRenderer 在 renderer 层面平级、互不依赖。
# 定位：全盘接管并升级 Object3DRenderer 的静态 PBR 能力为「同 mesh 摆 N 份 = 一次
# instanced draw」；过渡期**刻意复制** Object3DRenderer 的 PBR/阴影片元与光照 uniform
# 上传（见头文件的过渡期同步约定）。摆放走 per-instance attribute（InstanceBuffer 基建）。
cc_library(
    name = "instanced_renderer",
    srcs = ["instanced/instanced_object_renderer.cc"],
    hdrs = ["instanced/instanced_object_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":instance_buffer",
        ":mesh_manager",
        ":shader_manager",
        ":texture_manager",
        ":texture_units",
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

# Primitives3DRenderer — 3D 图元渲染（三角形/条带/线段/文本）+ MVP 矩阵构建
cc_library(
    name = "primitives3d_renderer",
    srcs = ["primitives3d/primitives3d_renderer.cc"],
    hdrs = ["primitives3d/primitives3d_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
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

# Primitives2DRenderer — 2D 图元绘制（Rect/Circle/Polyline/RoundRect/FillRect/Arc/Strip2D/Image2D）
cc_library(
    name = "primitives2d_renderer",
    srcs = ["primitives2d/primitives2d_renderer.cc"],
    hdrs = ["primitives2d/primitives2d_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":texture_manager",
        "//tools/jpov/interface:render_command",
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

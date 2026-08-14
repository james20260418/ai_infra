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
        "//third_party/tinygltf",
        "//third_party/stb:stb_image",
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
    copts = ["-std=c++20"],
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
        ":primitives3d_renderer",
        ":shader_manager",
        ":texture_manager",
        ":gltf_loader",
        ":orm_unpack",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/interface:window_info",
        "//tools/jpov/interface:gltf_object",
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
    copts = ["-std=c++20", "-DSTB_IMAGE_WRITE_IMPLEMENTATION"],
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

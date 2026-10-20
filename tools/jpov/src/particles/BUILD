load("@rules_cc//cc:defs.bzl", "cc_library")

# 广告牌粒子火 —— 模拟内核（GL-free、确定性、可单测）。
# 设计见 tools/jpov/docs/jpov_particle_fire_design.md。
cc_library(
    name = "particle_system",
    srcs = ["particle_system.cc"],
    hdrs = ["particle_system.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "//tools/jpov/interface:particle_types",
        "//tools/jpov/interface:pbr_material",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# 广告牌粒子火 —— 渲染器（instanced 广告牌面片 + flipbook + 加法/半透明混合）。
cc_library(
    name = "particle_renderer",
    srcs = ["particle_renderer.cc"],
    hdrs = ["particle_renderer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":particle_system",
        "//tools/jpov/interface:camera",
        "//tools/jpov/interface:particle_types",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/src:instance_buffer",
        "//tools/jpov/src:shader_manager",
        "//tools/jpov/src/wbo:wbo_shader",
        "//tools/jpov/src/wbo:wbo_target",
        "//tools/jpov/src:texture_manager",
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

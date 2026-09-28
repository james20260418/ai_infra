load("@rules_cc//cc:defs.bzl", "cc_library")

# FireParticleEmitter — CPU 粒子模拟（header-only、GL-free）：粒子池 + 发射 +
# 上升/摆动 + 寿命回收 → 产出 FireCommand 快照。火焰/燃烧共用。
cc_library(
    name = "fire_particles",
    hdrs = ["fire_particles.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov/interface:render_command",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

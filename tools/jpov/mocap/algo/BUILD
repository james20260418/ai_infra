load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ============================================================================
# JPOV Mocap — 动捕算法独立落点（GL-free / CPU）
#
# 目录职责：
#   mocap_types.h        外部 track / 方向场 / fit 结果的数据契约
#   mocap_landmarks.h    MediaPipe 33 点 → 22 根骨段 的唯一骨段表
#   mocap_math.h         零相位平滑 / 中位数 / 归一化（算法与 fit 共用）
#   mocap_algorithms.*   方向场算法注册表（worldz / worldz_t / lift2d / lift2d_t）
#   mocap_fit.*          方向场 → Mixamo23 pose（整体 yaw + 根位移 + 脚钉地）
#   mocap_io.*           track / poses / 批量报告 JSON 读写
#
# 这一层不依赖 GL，也不依赖 demo；viewer 侧只消费 poses.json / FBX。
# ============================================================================

cc_library(
    name = "mocap_types",
    hdrs = ["mocap_types.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "//tools/jpov/interface:mixamo23_skeleton",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mocap_math",
    hdrs = ["mocap_math.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mocap_landmarks",
    hdrs = ["mocap_landmarks.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mocap_types",
        "//geom/common:vec",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mocap_algorithms",
    srcs = ["mocap_algorithms.cc"],
    hdrs = ["mocap_algorithms.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mocap_landmarks",
        ":mocap_math",
        ":mocap_types",
        "//geom/common:vec",
        "//tools/jpov/interface:mixamo23_skeleton",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mocap_fit",
    srcs = ["mocap_fit.cc"],
    hdrs = ["mocap_fit.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mocap_landmarks",
        ":mocap_math",
        ":mocap_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "//tools/jpov/interface:mixamo23_skeleton",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "mocap_io",
    srcs = ["mocap_io.cc"],
    hdrs = ["mocap_io.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mocap_types",
        "//third_party/nlohmann-json:json",
        "//tools/jpov/interface:mixamo23_skeleton",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_mocap_fit",
    srcs = ["mocap_fit_main.cc"],
    deps = [
        ":mocap_algorithms",
        ":mocap_fit",
        ":mocap_io",
        ":mocap_types",
        "//tools/jpov/interface:mixamo23_skeleton",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

# 纯 CPU 单测：方向场（T-pose 不变式 / lift2d 深度符号 / 平滑不滞后）+
# fit（yaw 90° / 髋平移 / 钉地）+ 指标（已知误差的合成 track）。
cc_test(
    name = "mocap_fit_test",
    srcs = ["mocap_fit_test.cc"],
    deps = [
        ":mocap_algorithms",
        ":mocap_fit",
        ":mocap_landmarks",
        ":mocap_types",
        "//geom/common:quaternion",
        "//geom/common:vec",
        "//tools/jpov/interface:mixamo23_skeleton",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

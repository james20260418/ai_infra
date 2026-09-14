load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# ============================================================================
# JPOV FBX 观察器（独立小工具）— 骨架/动作验证载体
#
# 把一段 FBX 动画按 fbx 自身的 fps 播成**火柴人**，肉眼验证"从 FBX 的 Lcl Rotation
# 构造出的 skeleton pose 合不合法"。场景与查看器/编辑器同款（sunny day 光照 +
# 右键环绕相机，复用 //tools/jpov:view_config），面板换成播放控制（暂停 + rest 模式）。
#
# 目录自洽：本工具的全部代码/测试都在 demo/fbx_viewer/ 下。
#   fbx_viewer_app : 渲染核心 App（场景 + 位姿采样 + 播放面板 + 时间推进）
#   jpov_fbx_viewer: 主程序（交互事件循环 / headless 单帧出图）
#
# 构建：./tools/jpov/build_jpov_fbx_viewer.sh
# 详见 tools/jpov/docs/jpov_fbx_viewer_design.md
# ============================================================================

cc_library(
    name = "fbx_viewer_app",
    hdrs = [
        "fbx_viewer_app.h",
        "skeleton_pose_transfer.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tools/jpov:view_config",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/interface:animation_sampler",
        "//tools/jpov/interface:mesh",
        "//tools/jpov/interface:skeleton_mesh",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/interface:ui",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_binary(
    name = "jpov_fbx_viewer",
    srcs = ["jpov_fbx_viewer.cc"],
    deps = [
        ":fbx_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "@glog_static//:glog",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 目标骨架 pose 直搬（无重定向对照组）单测：骨名匹配 / 未命中保持 identity /
# 命中骨数值**原样**（不含任何重定向校正）—— 纯 CPU。
cc_test(
    name = "skeleton_pose_transfer_test",
    srcs = ["skeleton_pose_transfer_test.cc"],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
    ],
    deps = [
        ":fbx_viewer_app",
        "//geom/common:quaternion",
        "//tools/jpov/interface:skeleton_types",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# 播放控制回归（白盒 friend FbxViewerPlaybackTest）：
#   - 时间推进只在"播放中"发生（暂停/rest 模式必须停住 —— 需求"按下后主频的时间停止更新"）；
#   - 位姿来源正确（rest 模式 = identity；动画模式 = 按时间采样，且时间推进后位姿真的变）。
# 纯 CPU：不 Init()、不碰 GL（直接构造 App 调私有逻辑，同 editor_drag_ownership_test 模式）。
cc_test(
    name = "fbx_viewer_playback_test",
    srcs = ["fbx_viewer_playback_test.cc"],
    deps = [
        ":fbx_viewer_app",
        "//geom/common:quaternion",
        "//tools/jpov/interface:animation_sampler",
        "//tools/jpov/interface:skeleton_types",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

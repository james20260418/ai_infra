# tools/jpov/test/fbx_viewer — FBX 观察器的固定帧渲染 gold
#
# 与其它 gold 同套路：generator(cc_binary) 写仓库 gold PNG，test(cc_test) 渲同一帧比对。
# 特殊之处（也是本测试的价值）：**渲染的就是观察器本体**（//tools/jpov/demo/fbx_viewer
# 的 FbxViewerApp，headless + 不画面板），不是另写一份"长得像"的场景 —— 故 gold 内容与
# 用户跑起来看到的画面一致，generator/test 也不可能分叉（共用 jpov_fbx_pose_gold_common.h）。
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_binary(
    name = "jpov_fbx_pose_gold_generator",
    srcs = [
        "jpov_fbx_pose_gold_generator.cc",
        "jpov_fbx_pose_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
    ],
    deps = [
        "//tools/jpov/demo/fbx_viewer:fbx_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_fbx_retarget_units_test",
    srcs = ["jpov_fbx_retarget_units_test.cc"],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
    ],
    deps = [
        "//tools/jpov/interface:skeleton_retarget",
        "//tools/jpov/src:fbx_loader",
        "//tools/jpov/src:gltf_loader",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

cc_test(
    name = "jpov_fbx_pose_gold_test",
    srcs = [
        "jpov_fbx_pose_gold_test.cc",
        "jpov_fbx_pose_gold_common.h",
    ],
    data = [
        "//tools/jpov/test/animations:hip_hop_dance_fbx_data",
        "//tools/jpov/test/object3d:mixamo_male_glb_data",
        ":fbx_dance_frame_1280x720.png",
        ":fbx_pose_glb_naive_1280x720.png",
        ":fbx_pose_glb_retarget_1280x720.png",
    ],
    deps = [
        "//tools/jpov/demo/fbx_viewer:fbx_viewer_app",
        "//tools/jpov/include/jpov:jpov",
        "//third_party/stb:stb_image",
        "//tools/jpov/test:compare_light",
        "//tools/jpov/test:test_utils",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

load("@rules_cc//cc:defs.bzl", "cc_test")

# JPOV UI 组件自证测试（无窗口，纯 CPU 指令层比对）。
# 后续 S1~S8 的控件测试与交互 demo main 也将放在本目录。
cc_test(
    name = "jpov_ui_s0_test",
    srcs = ["jpov_ui_s0_test.cc"],
    deps = [
        "//tools/jpov/interface:ui",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# S1 基础控件（Text / ColorSwatch）自证测试。
# 纯 CPU 指令层比对，无窗口，无需 link 渲染器。
cc_test(
    name = "jpov_ui_s1_test",
    srcs = ["jpov_ui_s1_test.cc"],
    deps = [
        "//tools/jpov/interface:ui",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

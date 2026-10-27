load("@rules_cc//cc:defs.bzl", "cc_test")

# sp_ui gold 测试（纯 CPU 指令层，无窗口、无 GL）。
# 钉住 sp_ui 独有且已真出过 bug 的机理：文字落在自己控件上（二次缩放回归）、
# 缩放处处同步、两层材质独立、屏幕锚定 UV、时间即数据、flipbook wrap、退化输入。
cc_test(
    name = "sp_ui_gold_test",
    srcs = ["sp_ui_gold_test.cc"],
    deps = [
        "//tools/jpov:sp_ui_demo_panel",
        "//tools/jpov/interface:render_command",
        "//tools/jpov/sp_ui:sp_ui",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

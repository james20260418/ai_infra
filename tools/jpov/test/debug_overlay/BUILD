load("@rules_cc//cc:defs.bzl", "cc_test")

# DebugInfo 文本契约（纯 CPU）：输出必须是内置点阵字体能画的纯 ASCII。
cc_test(
    name = "debug_info_test",
    srcs = ["debug_info_test.cc"],
    deps = [
        "//tools/jpov/interface:debug_info",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# 版面测量纯函数单测（零 GL）：首行为空/多字节/行尾换行这些形状都靠它钉住。
cc_test(
    name = "overlay_extent_test",
    srcs = ["overlay_extent_test.cc"],
    deps = [
        "//tools/jpov/src:debug_overlay_headers",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# Debug HUD GPU 像素测试：在**零字体注册**的 app 上验证内置点阵字体照样画出来、
# 落在左上角、且污染不到 HUD 之外的像素。
# target_compatible_with 与其他 GPU 测试一致：只在 Linux x86_64（CI 是 llvmpipe）跑。
cc_test(
    name = "debug_overlay_gpu_test",
    srcs = ["debug_overlay_gpu_test.cc"],
    deps = [
        "//third_party/stb:stb_image",
        "//tools/common:utils",
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:debug_overlay_renderer",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

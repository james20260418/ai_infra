load("@rules_cc//cc:defs.bzl", "cc_test")

# 水体单次散射：闭式积分 vs 4096 步数值积分对照（纯 CPU，无 GL）。
# 钉住的是"指数项的符号/量纲"——这类错误在画面上只表现为"深处反常变亮"，肉眼难判。
cc_test(
    name = "water_scattering_test",
    srcs = ["water_scattering_test.cc"],
    deps = [
        "//tools/jpov/src/water:water_scattering",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

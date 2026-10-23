# tools/jpov/tools/util —— 工具侧通用几何/算法件。
#
# 这里放的是「工具（离线流程）需要、但引擎运行时不需要」的东西；引擎侧几何请放 geom/。
# 若某件东西后来被引擎也要用，整体挪到 geom/ 对应目录，不要在两处各长一份。

package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# NearestPointMatcher3d —— 「点到最近折线点」查询（稀疏栅格加速，header-only，模板）。
# 设计取舍（为什么是点到线段、环形扩张的停止判据、盒外查询点怎么夹回）见头文件注释。
cc_library(
    name = "nearest_point_matcher_3d",
    hdrs = ["nearest_point_matcher_3d.h"],
    deps = [
        "//geom/common:check",
        "//geom/common:grid",
        "//geom/common:grid_map",
        "//geom/common:vec",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "nearest_point_matcher_3d_test",
    srcs = ["nearest_point_matcher_3d_test.cc"],
    deps = [
        ":nearest_point_matcher_3d",
        "//geom/common:vec",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
    size = "medium",
)

# tools/jpov/tools/vegetation_tool_v2/gen —— 导出链的 C++ 实现。
#
# 分工（详见 dump_sapling_curve.py 与 shape_builder.h 的文件头）：
#   Blender 只负责把 Sapling 建出来的树倒成 .dump（曲线控制点 + 叶卡网格顶点）；
#   采样 / 父枝 / 叶卡姿态 / 拓扑序 / 写 shape.vegshape（或 --text）全在这里。
#
# 为什么搬：原 Python 版「给每条样条找父枝」是 O(枝数²·段数) 的三重循环，
# fir 那棵树（4100 枝 / 68944 段）光这一步 119 s；换成稀疏栅格最近邻后是毫秒级。

package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "sapling_dump",
    srcs = ["sapling_dump.cc"],
    hdrs = ["sapling_dump.h"],
    deps = [
        "//tools/jpov/tools/vegetation_tool_v2:shape",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "shape_builder",
    srcs = ["shape_builder.cc"],
    hdrs = ["shape_builder.h"],
    deps = [
        ":sapling_dump",
        "//geom/common:vec",
        "//tools/jpov/tools/util:nearest_point_matcher_3d",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
        # matcher 的 CHECK 需要 glog（头文件里的 CHECK_* 不是纯声明）。
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "shape_text",
    srcs = ["shape_text.cc"],
    hdrs = ["shape_text.h"],
    deps = [
        "//tools/jpov/tools/vegetation_tool_v2:shape",
    ],
    copts = ["-std=c++20"],
)

# 命令行入口：配置 → 调 Blender 倒数据 → 建 shape → 写 pbtxt。
cc_binary(
    name = "shape_gen",
    srcs = ["shape_gen.cc"],
    data = [
        "//tools/jpov/tools/vegetation_tool_v2:dump_sapling_curve.py",
    ],
    deps = [
        ":sapling_dump",
        ":shape_builder",
        ":shape_text",
        "//tools/jpov/tools/vegetation_tool_v2:generate_proto",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
        "@com_google_protobuf//:protobuf",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "sapling_dump_test",
    srcs = ["sapling_dump_test.cc"],
    deps = [
        ":sapling_dump",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "shape_builder_test",
    # glog 是静态库；测试若走 .so 会在运行时找不到 CheckOpMessageBuilder 符号。
    linkstatic = True,
    srcs = ["shape_builder_test.cc"],
    deps = [
        ":sapling_dump",
        ":shape_builder",
        ":shape_text",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# 资产格式互转：*.vegshape（二进制，标准）/ *.pbtxt（文本，只给人看）。
cc_binary(
    name = "shape_convert",
    srcs = ["shape_convert.cc"],
    deps = [
        ":shape_text",
        "//tools/jpov/tools/vegetation_tool_v2:shape",
    ],
)

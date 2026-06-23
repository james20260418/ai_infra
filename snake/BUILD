package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# 贪吃蛇数据结构
cc_library(
    name = "snake_types",
    hdrs = ["snake_types.h"],
    copts = ["-std=c++20"],
)

# 贪吃蛇游戏逻辑（不含渲染）
cc_library(
    name = "snake_logic",
    srcs = ["snake_logic.cc"],
    hdrs = ["snake_logic.h"],
    deps = [":snake_types"],
    copts = ["-std=c++20"],
)

# 贪吃蛇逻辑单元测试
cc_test(
    name = "snake_logic_test",
    srcs = ["snake_logic_test.cc"],
    deps = [
        ":snake_logic",
        ":snake_types",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# 贪吃蛇渲染器（独立的 JPOV 2D 渲染逻辑）
cc_library(
    name = "snake_renderer",
    srcs = ["snake_renderer.cc"],
    hdrs = ["snake_renderer.h"],
    deps = [
        ":snake_types",
        "//tools/jpov:jpov",
    ],
    copts = ["-std=c++20"],
)

# 贪吃蛇游戏应用（JPOV 视图控制器 + 输入处理）
cc_library(
    name = "snake_app",
    srcs = ["snake_app.cc"],
    hdrs = ["snake_app.h"],
    deps = [
        ":snake_logic",
        ":snake_renderer",
        ":snake_types",
        "//tools/jpov:jpov",
    ],
    copts = ["-std=c++20"],
    linkopts = ["-lGL"],
    linkstatic = 1,
)

# 贪吃蛇运行入口
cc_binary(
    name = "snake_game",
    srcs = ["main.cc"],
    deps = [":snake_app"],
)

# 贪吃蛇自测验收程序（使用 JPOV RunOnce 截图）
cc_binary(
    name = "snake_check_acceptance",
    srcs = ["snake_check_acceptance.cc"],
    deps = [":snake_app"],
    copts = ["-std=c++20"],
)

# 食物生成和吃食物增长验收测试
cc_test(
    name = "food_acceptance_test",
    srcs = ["food_acceptance_test.cc"],
    deps = [
        ":snake_app",
        ":snake_logic",
        ":snake_types",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

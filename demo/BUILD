package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary")

# GLFW 集成测试 — 验证 GLFW 编译链接和基本 API
cc_binary(
    name = "glfw_test",
    srcs = ["glfw_test.cpp"],
    copts = ["-std=c++20"],
    linkopts = [
        "-lglfw",
        "-lGL",
    ],
)

# Windows Hello World demo — cross-compiled with MinGW
# Name ends with .exe for Windows output
cc_binary(
    name = "windows_helloworld.exe",
    srcs = ["windows_helloworld.cpp"],
    deps = [
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

genrule(
    name = "copy_windows_helloworld",
    srcs = [":windows_helloworld.exe"],
    outs = ["windows_helloworld_output.exe"],
    cmd = "cp $< $@",
)

package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary")

# GLFW 集成测试 — 验证 GLFW 编译链接和基本 API
cc_binary(
    name = "glfw_test",
    srcs = ["glfw_test.cpp"],
    deps = ["@glfw_static//:glfw"],
    copts = ["-std=c++20"],
    linkopts = ["-lGL"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# GLFW 点击响应演示 — 可见窗口，响应鼠标点击
cc_binary(
    name = "glfw_click",
    srcs = ["glfw_click.cpp"],
    deps = ["@glfw_static//:glfw"],
    copts = ["-std=c++20"],
    linkopts = ["-lGL"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

# GLFW 点击响应演示 — Windows 版（MinGW 交叉编译，静态链接）
cc_binary(
    name = "glfw_click.exe",
    srcs = ["glfw_click.cpp"],
    deps = ["@glfw_mingw//:glfw"],
    copts = ["-std=c++20"],
    linkopts = ["-lopengl32"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

genrule(
    name = "copy_glfw_click",
    srcs = [":glfw_click"],
    outs = ["glfw_click_output"],
    cmd = "cp $< $@",
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

genrule(
    name = "copy_glfw_click_win",
    srcs = [":glfw_click.exe"],
    outs = ["glfw_click_output.exe"],
    cmd = "cp $< $@",
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

# Windows Hello World demo — cross-compiled with MinGW
cc_binary(
    name = "windows_helloworld.exe",
    srcs = ["windows_helloworld.cpp"],
    deps = [
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

genrule(
    name = "copy_windows_helloworld",
    srcs = [":windows_helloworld.exe"],
    outs = ["windows_helloworld_output.exe"],
    cmd = "cp $< $@",
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

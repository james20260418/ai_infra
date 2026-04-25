package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_binary")

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

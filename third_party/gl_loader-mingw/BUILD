package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library")

# Lightweight OpenGL function loader for MinGW/Windows.
# Uses wglGetProcAddress to load GL 1.1+ functions at runtime.
# On Linux/Mesa, this library is not needed.

cc_library(
    name = "gl_loader",
    srcs = ["gl_loader.c"],
    hdrs = ["gl_loader.h"],
    linkstatic = 1,
    linkopts = ["-lopengl32", "-lgdi32"],
    copts = ["-std=c99"],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

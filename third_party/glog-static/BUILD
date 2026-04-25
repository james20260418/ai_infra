package(default_visibility = ["//visibility:public"])

# Platform-specific glog static library
# Selects the correct .a depending on the target platform.
cc_library(
    name = "glog",
    srcs = select({
        "@platforms//os:linux": ["lib/linux-x86_64/libglog.a"],
        "@platforms//os:windows": ["lib/windows-x86_64/libglog.a"],
        "//conditions:default": ["lib/linux-x86_64/libglog.a"],
    }),
    hdrs = glob(["include/glog/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-lpthread"],
    }),
)

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "glog",
    srcs = ["lib/libglog.a"],
    hdrs = glob(["include/glog/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    visibility = ["//visibility:public"],
    linkopts = ["-lpthread"],
)

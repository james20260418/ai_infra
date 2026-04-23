package(default_visibility = ["//visibility:public"])

cc_library(
    name = "glog",
    srcs = [
        "lib/libglog.a",
        "lib/libgflags.a",
    ],
    hdrs = glob(["include/glog/*.h", "include/gflags/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    linkopts = ["-lpthread"],
)

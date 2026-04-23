package(default_visibility = ["//visibility:public"])

cc_library(
    name = "glog",
    srcs = ["lib/libglog.a"],
    hdrs = glob(["include/glog/*.h", "include/gflags/*.h"]),
    includes = ["include"],
    deps = ["@com_github_gflags_gflags//:gflags"],
    linkstatic = 1,
    linkopts = ["-lpthread"],
)

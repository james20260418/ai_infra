package(default_visibility = ["//visibility:public"])

cc_library(
    name = "glog",
    srcs = ["lib/libglog.a"],
    hdrs = glob(["include/glog/*.h"]),
    includes = ["include"],
    linkstatic = 1,  # 1 means static linking
    linkopts = ["-lgflags", "-lpthread"],  # glog requires gflags and pthread
)
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "protobuf",
    srcs = ["lib/libprotobuf.a"],
    hdrs = glob(["include/google/protobuf/*.proto.descriptor.inc"]),
    includes = ["include"],
    linkstatic = 1,
    visibility = ["//visibility:public"],
)

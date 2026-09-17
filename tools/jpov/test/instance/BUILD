# tools/jpov/test/instance — Instanced draw 通路的不变量测试
#
# instance_buffer_test：守「per-instance 属性挂载/摘除严格配对」——
#   Attach 后必须 enabled+divisor=1（负向参照），Detach / RAII 出作用域后必须全清。
#   防的是残留 divisor=1 启用态泄漏给后续普通 draw（重构前真实存在的缺陷）。
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "instance_buffer_test",
    srcs = ["instance_buffer_test.cc"],
    deps = [
        "//tools/jpov/include/jpov:jpov",
        "//tools/jpov/src:instance_buffer",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

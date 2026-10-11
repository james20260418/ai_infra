# tools/jpov/test/instance — Instanced draw 通路的不变量测试
#
# instance_buffer_test：守「per-instance 属性挂载/摘除严格配对」——
#   Attach 后必须 enabled+divisor=1（负向参照），Detach / RAII 出作用域后必须全清。
#   防的是残留 divisor=1 启用态泄漏给后续普通 draw（重构前真实存在的缺陷）。
#
# instanced_object_attrs_test：守静态实例渲染器 per-instance 布局 spec 的自洽性
#   （spec 合法 / 两两不重叠 / 不侵占顶点属性槽）+ 谓词的负向参照（非恒真）。
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

cc_test(
    name = "instanced_object_attrs_test",
    srcs = ["instanced_object_attrs_test.cc"],
    deps = [
        "//tools/jpov/src:instance_buffer",
        "//tools/jpov/src:instanced_object_attrs",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
)

# gen3d — 文本 prompt → 静态 PBR GLB → JPOV 渲染验证（自动生成 3D 模型工具链）
#
# 库结构（延续 gen3d_config.h 的供应商无关设计）：
#   gen3d_config.h — Gen3dConfig（已有，纯语义，不绑供应商）
#   tripo_client.h — TripoClient：唯一直接发 HTTP / 解析 tripo3d 响应的类
#   http: //third_party/curl-static:curl    json: //third_party/nlohmann-json:json

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "gen3d",
    srcs = ["tripo_client.cc"],
    hdrs = [
        "gen3d_config.h",
        "tripo_client.h",
    ],
    deps = [
        "//third_party/curl-static:curl",
        "//third_party/nlohmann-json:json",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
)

# gen3d_cmd：文本 prompt → tripo3d 生成 GLB（Linux ELF）。
# 渲染出图交给 jpov_model_viewer --four_views，分工见 docs/jpov_gen3d_design.md。
cc_binary(
    name = "gen3d_cmd",
    srcs = ["gen3d_cmd.cc"],
    deps = [
        ":gen3d",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

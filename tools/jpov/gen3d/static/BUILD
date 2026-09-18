# gen3d/static — 文本 prompt → 静态 PBR GLB → JPOV 渲染验证（自动生成静态 3D 模型工具链）
#
# gen3d 顶层目录拆两层：static/（本目录，静态 PBR 模型）与 skeleton/（带骨骼，见 sibling）。
# 本目录是迁移自旧 tools/jpov/gen3d/ 的内容，bazel target 路径从
#   //tools/jpov/gen3d:gen3d  /  :gen3d_cmd
# 变为
#   //tools/jpov/gen3d/static:gen3d  /  :gen3d_cmd
#
# 库结构（延续 gen3d_config.h 的供应商无关设计）：
#   gen3d_config.h — Gen3dConfig（已有，纯语义，不绑供应商）
#   image_input.h  — 生成输入图的本地格式审查（避免浪费 credit，纯函数无 HTTP）
#   tripo_client.h — TripoClient：唯一直接发 HTTP / 解析 tripo3d 响应的类
#   http: //third_party/curl-static:curl    json: //third_party/nlohmann-json:json
#   stb : //third_party/stb:stb_image（image_input 解码取尺寸/查 alpha）

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "gen3d",
    srcs = [
        "image_input.cc",
        "tripo_client.cc",
    ],
    hdrs = [
        "gen3d_config.h",
        "image_input.h",
        "tripo_client.h",
    ],
    deps = [
        "//third_party/curl-static:curl",
        "//third_party/nlohmann-json:json",
        "//third_party/stb:stb_image",
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

# image_input_test：守「提交前本地格式审查」的判定正确性。
# 重点测“能不能真的拦住”（负向用例）与“会不会误拦合法图”（正向用例），
# 并守住 alpha 报告字段的真伪（这是底稿交付方案的判据依据）。
cc_test(
    name = "image_input_test",
    srcs = [
        "image_input_test.cc",
        # stb_image_write 的"唯一实现单元"（该目标 header-only，需要一处 include
        # 才能真正产出 stbi_write_* 符号；生产侧由 tools/jpov/src/orm_unpack.cc 承担，
        # 本测试不链接那个库，故自带一份）。
        "image_input_test_stb_impl.cc",
    ],
    deps = [
        ":gen3d",
        "//third_party/stb:stb_image_write",
        "@glog_static//:glog",
    ],
    copts = ["-std=c++20"],
    linkstatic = 1,
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

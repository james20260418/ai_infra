# GoogleTest —— **随仓库携带**（third_party/googletest-release-1.11.0/，v1.11.0 原始源码）。
#
# 为什么不用 http_archive：那会让「首次构建」必须联网（干净缓存 / CI / 新机器）。
# 本仓库的定位是**离线可复现**：除了 protobuf 自带的依赖链（见 README「外部依赖」），
# 这里不再有任何 http_archive。
local_repository(
    name = "gtest",
    path = "third_party/googletest-release-1.11.0",
)

# gflags 曾被声明为 "required by glog"，但本仓库的 glog 是**预编译静态库**
# （third_party/glog-static/），构建期不需要 gflags；全仓也没有任何
# `@com_github_gflags_gflags//:...` 引用。已删除，避免无谓的网络下载。

# Local glog static library dependency
local_repository(
    name = "glog_static",
    path = "third_party/glog-static",
)

# Local protobuf dependency (source build)
local_repository(
    name = "com_google_protobuf",
    path = "third_party/protobuf-3.14.0-rc3",
)

# Load protobuf dependencies
load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()

# Register protobuf toolchain
load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies", "rules_proto_toolchains")
rules_proto_dependencies()
rules_proto_toolchains()

# Local GLFW static library (MinGW cross-compiled, for Windows)
new_local_repository(
    name = "glfw_mingw",
    path = "third_party/glfw-mingw",
    build_file = "third_party/glfw-mingw/BUILD",
)

# Local GLFW static library dependency (3.3.10, from source)
new_local_repository(
    name = "glfw_static",
    path = "third_party/glfw-static",
    build_file = "third_party/glfw-static/BUILD",
)

# Local OpenCV static library dependency
new_local_repository(
    name = "opencv_static",
    path = "third_party/opencv-static",
    build_file = "third_party/opencv-static/BUILD",
)

# Set environment variables for warning suppression
# These will be picked up by the compiler
register_toolchains("@local_config_cc//:all")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

# GoogleTest dependency
http_archive(
    name = "gtest",
    sha256 = "b4870bf121ff7795ba20d20bcdd8627b8e088f2d1dab299a031c1034eddc93d5",
    strip_prefix = "googletest-release-1.11.0",
    urls = [
        "https://github.com/google/googletest/archive/refs/tags/release-1.11.0.tar.gz",
    ],
)

# gflags dependency (required by glog)
http_archive(
    name = "com_github_gflags_gflags",
    sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
    strip_prefix = "gflags-2.2.2",
    urls = [
        "https://mirror.bazel.build/github.com/gflags/gflags/archive/v2.2.2.tar.gz",
        "https://github.com/gflags/gflags/archive/v2.2.2.tar.gz",
    ],
)

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

# Set environment variables for warning suppression
# These will be picked up by the compiler
register_toolchains("@local_config_cc//:all")

package(default_visibility = ["//visibility:public"])

load(":mingw_toolchain_config.bzl", "mingw_toolchain_config")

# The toolchain config provider for MinGW
mingw_toolchain_config(name = "mingw_cc_toolchain_config")

# The cc_toolchain that wires everything together
cc_toolchain(
    name = "mingw_cc_toolchain",
    toolchain_identifier = "mingw-w64-x86_64",
    toolchain_config = ":mingw_cc_toolchain_config",
    all_files = ":empty",
    ar_files = ":empty",
    as_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
)

# Register this as a Bazel toolchain for the platform
toolchain(
    name = "mingw_windows_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
    toolchain = ":mingw_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)

filegroup(name = "empty")

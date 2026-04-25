"""Starlark rule for MinGW cross-compilation toolchain config.

Targets Windows x86_64 from Linux host using system-installed
x86_64-w64-mingw32-g++ (MinGW-w64).
"""

load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "feature", "flag_set", "flag_group", "tool_path", "with_feature_set")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

def _impl(ctx):
    toolchain_identifier = "mingw-w64-x86_64"

    host_system_name = "x86_64-linux"
    target_system_name = "x86_64-w64-mingw32"
    target_cpu = "x86_64"
    target_libc = "mingw"
    compiler = "gcc"

    action_names = ACTION_NAMES

    # Tool paths: the cross-compiler binaries installed on the host
    tool_paths = [
        tool_path(name = "gcc", path = "/usr/bin/x86_64-w64-mingw32-g++"),
        tool_path(name = "ld", path = "/usr/bin/x86_64-w64-mingw32-ld"),
        tool_path(name = "ar", path = "/usr/bin/x86_64-w64-mingw32-ar"),
        tool_path(name = "cpp", path = "/usr/bin/x86_64-w64-mingw32-cpp"),
        tool_path(name = "gcov", path = "/usr/bin/x86_64-w64-mingw32-gcov"),
        tool_path(name = "nm", path = "/usr/bin/x86_64-w64-mingw32-nm"),
        tool_path(name = "objdump", path = "/usr/bin/x86_64-w64-mingw32-objdump"),
        tool_path(name = "strip", path = "/usr/bin/x86_64-w64-mingw32-strip"),
    ]

    # Default compile flags
    default_compile_flags_feature = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    action_names.assemble,
                    action_names.preprocess_assemble,
                    action_names.linkstamp_compile,
                    action_names.c_compile,
                    action_names.cpp_compile,
                    action_names.cpp_header_parsing,
                    action_names.cpp_module_compile,
                    action_names.cpp_module_codegen,
                    action_names.lto_backend,
                    action_names.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-std=c++20",
                            "-Wall",
                            "-Wextra",
                            "-Wno-stringop-overflow",
                            "-Wno-implicit-fallthrough",
                            "-Wno-array-bounds",
                            "-Wno-maybe-uninitialized",
                            "-Wno-unused-parameter",
                            "-Wno-sign-compare",
                            "-Wno-deprecated-declarations",
                            "-Wno-class-memaccess",
                            "-Wno-cast-function-type",
                            "-Wno-unused-but-set-parameter",
                            "-Wno-free-nonheap-object",
                        ],
                    ),
                ],
            ),
        ],
    )

    # Default link flags
    default_link_flags_feature = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    action_names.cpp_link_executable,
                    action_names.cpp_link_dynamic_library,
                    action_names.cpp_link_nodeps_dynamic_library,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-static",
                            "-lpthread",
                        ],
                    ),
                ],
            ),
        ],
    )

    # User compile flags (from --copt)
    user_compile_flags_feature = feature(
        name = "user_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    action_names.assemble,
                    action_names.preprocess_assemble,
                    action_names.c_compile,
                    action_names.cpp_compile,
                    action_names.cpp_header_parsing,
                    action_names.cpp_module_compile,
                    action_names.cpp_module_codegen,
                    action_names.lto_backend,
                    action_names.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        iterate_over = "user_compile_flags",
                        flags = ["%{user_compile_flags}"],
                    ),
                ],
            ),
        ],
    )

    # User link flags (from --linkopt)
    user_link_flags_feature = feature(
        name = "user_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    action_names.cpp_link_executable,
                    action_names.cpp_link_dynamic_library,
                    action_names.cpp_link_nodeps_dynamic_library,
                ],
                flag_groups = [
                    flag_group(
                        iterate_over = "user_link_flags",
                        flags = ["%{user_link_flags}"],
                    ),
                ],
            ),
        ],
    )

    features = [
        default_compile_flags_feature,
        default_link_flags_feature,
        user_compile_flags_feature,
        user_link_flags_feature,
    ]

    # Built-in include directories from MinGW
    cxx_builtin_include_directories = [
        "/usr/x86_64-w64-mingw32/include",
        "/usr/x86_64-w64-mingw32/include/c++",
        "/usr/x86_64-w64-mingw32/include/c++/x86_64-w64-mingw32",
        "/usr/x86_64-w64-mingw32/include/c++/backward",
        "/usr/lib/gcc/x86_64-w64-mingw32/13-win32/include",
        "/usr/lib/gcc/x86_64-w64-mingw32/13-win32/include-fixed",
        "/usr/share/mingw-w64/include",
    ]

    # Ensure the sysroot is available for the compiler
    builtin_sysroot = "/usr/x86_64-w64-mingw32"

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,
        toolchain_identifier = toolchain_identifier,
        host_system_name = host_system_name,
        target_system_name = target_system_name,
        target_cpu = target_cpu,
        target_libc = target_libc,
        compiler = compiler,
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        cxx_builtin_include_directories = cxx_builtin_include_directories,
    )

mingw_toolchain_config = rule(
    implementation = _impl,
    provides = [CcToolchainConfigInfo],
    attrs = {},
)

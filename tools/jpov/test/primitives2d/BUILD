load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_library(
    name = "test_utils",
    hdrs = ["test_utils.h"],
    visibility = ["//tools/jpov:__subpackages__"],
    deps = ["@glog_static//:glog"],
)

cc_test(name = "jpov_rect_gold_test", srcs = ["jpov_rect_gold_test.cc"], data = [":rect_centered_blue_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_polyline_gold_generator", srcs = ["jpov_polyline_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_polyline_gold_test", srcs = ["jpov_polyline_gold_test.cc"], data = [":polyline_zigzag_red_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//third_party/stb:stb_image"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_circle_gold_generator", srcs = ["jpov_circle_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_circle_gold_test", srcs = ["jpov_circle_gold_test.cc"], data = [":circle_centered_green_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_strip2d_gold_generator", srcs = ["jpov_strip2d_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_strip2d_gold_test", srcs = ["jpov_strip2d_gold_test.cc"], data = [":strip2d_diagonal_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_roundrect_gold_generator", srcs = ["jpov_roundrect_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_roundrect_gold_test", srcs = ["jpov_roundrect_gold_test.cc"], data = [":roundrect_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_fillrect_gold_generator", srcs = ["jpov_fillrect_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_fillrect_gold_test", srcs = ["jpov_fillrect_gold_test.cc"], data = [":fillrect_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_arc2d_gold_generator", srcs = ["jpov_arc2d_gold_generator.cc"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_arc2d_gold_test", srcs = ["jpov_arc2d_gold_test.cc"], data = [":arc2d_640x360.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

cc_binary(name = "jpov_image2d_gold_generator", srcs = ["jpov_image2d_gold_generator.cc"], data = [":stamp_200x200.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", "//tools/jpov/test:test_utils"], target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])
cc_test(name = "jpov_image2d_gold_test", srcs = ["jpov_image2d_gold_test.cc"], data = [":image2d_stamp_640x360.png", ":stamp_200x200.png"], deps = ["//tools/jpov/include/jpov:jpov", "//tools/common:utils", ":test_utils"], copts = ["-std=c++20"], linkstatic = 1, target_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"])

load("@rules_cc//cc:defs.bzl", "cc_binary")

# Fire-Fog tile culling 三法基准（临时 spike，验证后删）：AABB vs SAT vs 扫描线(ConvexHullTiles)
cc_binary(
    name = "fire_fog_hull_bench",
    srcs = ["fire_fog_hull_bench.cc"],
    deps = [
        "//geom/math:convex_hull_tiles",
    ],
    copts = ["-std=c++20"],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

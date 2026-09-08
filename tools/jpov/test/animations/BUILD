# tools/jpov/test/animations — 人类动作(动画)测试资材
#
# hip_hop_dance.fbx: Mixamo "Hip Hop Dancing"，以 mixamorig(或 Mixamo 骨架)导出的
#   全身动画 FBX。用途: 骨架子系统 LoadMixamoAnimation 的输入资材 + ufbx 读入验证。
#   (来源: 下载自 mixamo.com 角色动画库；含 1 段舞蹈动画。)
# filegroup 供 cc_test / cc_binary 以 data 引用(路径经 GetTestDataDir() 取)。

# filegroup 是 Bazel 内置 native 规则，无需从 @rules_cc//cc:defs 加载（rules_cc 没有导出它）。
filegroup(
    name = "hip_hop_dance_fbx_data",
    srcs = glob(["*.fbx"]),
    visibility = ["//tools/jpov:__subpackages__"],
)

# JPOV gen3d — 顶层说明

`gen3d` 是 JPOV 里「文本/模型 prompt → 经 Tripo 生成 3D GLB」的工具。顶层拆两层，
各配一个仓库根的编排脚本：

| 子目录 | 内容 | bazel target(示例) | 编排脚本 |
|---|---|---|---|
| `gen3d/static/`   | **静态 PBR 模型**生成（text-to-model，无骨骼） | `//tools/jpov/gen3d/static:gen3d_cmd` | `tools/jpov/gen3d_static.sh` |
| `gen3d/skeleton/` | **带骨骼**生成（rig / mixamorig 蒙皮 / 动画，规划中） | `tools/jpov/gen3d_skeleton.sh`（占位） | `tools/jpov/gen3d_skeleton.sh` |

- `gen3d/static/` 里更细节的使用说明见它的 `README.md`；架构设计见
  `docs/jpov_gen3d_design.md`（历史 + 迁移现状已在文档头部标注）。
- `gen3d/skeleton/` 当前是占位骨架（loader 尚未接 skins/animations，见
  `docs/jpov_clothes_rig_design.md`）。`gen3d_skeleton.sh` 占位会在调用时打印"未实现"退出，
  避免误触发真实生成。

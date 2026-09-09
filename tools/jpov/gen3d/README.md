# JPOV gen3d — 顶层说明

`gen3d` 是 JPOV 里「文本/模型 prompt → 经 Tripo 生成 3D GLB」的工具。顶层拆两层，
各配一个仓库根的编排脚本：

| 子目录 | 内容 | bazel target | 编排脚本 |
|---|---|---|---|
| `gen3d/static/`   | **静态 PBR 模型**生成（text-to-model，无骨骼） | `//tools/jpov/gen3d/static:gen3d_cmd` | `tools/jpov/gen3d_static.sh` |
| `gen3d/skeleton/` | **带骨骼**生成（prompt→Auto Rig spec=mixamo→带 mixamorig 骨架 GLB） | `//tools/jpov/gen3d/skeleton:gen3d_skeleton_cmd` | `tools/jpov/gen3d_skeleton.sh` |

- `gen3d/static/` 使用说明见其 `README.md`；`gen3d/skeleton/` 的见 `skeleton/README.md`。
  架构设计见 `docs/jpov_gen3d_design.md`（历史+迁移现状已头部标注）与 `docs/jpov_clothes_rig_design.md`。
- 渲染侧暂不驱动骨骼（loader 未接 skin）；`skeleton/` 先把「产出带骨骼 GLB」跑通，驱动/渲染验证在后续 PR。

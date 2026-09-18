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

## 生成样例（输入图 + 产物，仓库内可查）

真想动手前，**先看这两个已入库的实测样例**，能省一轮 credit 和一次踩坑：

| 样例目录 | 模式 | 结论 |
|---|---|---|
| `test/object3d/wallet_tripo_multiview/` | `--images`（4 视图） | ✅ **正面样例**：硬表面单体可用（结构完整、无破损）；含 4 张输入图 + GLB + 四视图渲染 |
| `test/object3d/oak_tripo_negative/` | `--image`（单图） | ❌ **反面教材**：单图重建复杂植物不可用（树冠碎成飘浮面片）；含输入图 + 4k/20k 两版 GLB + 渲染 |

两个样例各配 `README.md`，记录了完整命令、参数、验收结论与教训（含「低模必须先高模再减面」
「植物类该用 billboard 纸片」等结论）。对照看可直观理解**输入结构复杂度如何决定成败**。

> ⚠️ 这两个目录**只为留证/参照**存在，不承载 gold test 或渲染回归断言（外部生成资产
> 不可控，拿它做基准没有意义）。

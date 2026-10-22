# lantern 样例（Khronos 官方 lantern.glb 的两个版本）

| 文件 | 内容 | 用途 |
|---|---|---|
| `lantern.glb` | 原封不动（3 个 primitive 靠 node **translation** 摆位，坐标是 **cm** 量级：高 25.85） | 贴图/内嵌资源测试的原始素材（`jpov_gltf_lantern_gold_test` 用） |
| `lantern_m.glb` | 同上，但**坐标已换成米**（×0.01，高 **0.2585 m**）且三个部件的 node translation 已烘进顶点（装配正确） | 实际干活用（模型编辑器 / 断面缝合 / 场景摆放） |

生成方式（一次性，可复现）：

1. 对每个 `node.mesh` 的每个 primitive：`POSITION = (POSITION + node.translation) × 0.01`，同步更新 accessor 的 min/max；
2. `node.translation = [0,0,0]`；其余（材质 / 内嵌贴图 / node 旋转）保持不变。

> 为什么不直接改原文件：`lantern.glb` 的 gold 图（`test/object3d/lantern_1280x720.png`）是
> 按它原来的包围盒取景的，换掉会连带改 gold；本仓库当前 loader **不应用 node translation**
> （见 gltf_loader.cc 的说明），所以原文件渲染出来三个部件是叠在原点、悬空分离的。

# samples/ —— 入库的范本拓扑（`shape.pbtxt`）与样例烘焙产物

这些是**输入**（一棵树长什么样：枝 / 采样环 / 叶卡）。`assets/` 放材质（树皮、叶卡图），
`configs/` 放**生成配置**（喂 Blender/Sapling 的参数），三者互不重复。

```bash
# 用哪一份都可以（cwd = 已构建的查看器目录，见 ../../build_jpov_vegetation_v2_viewer.sh）
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --shape shapes/oak_low.pbtxt \
    --bark bark/bark001 --leaf leaf/willow_leaf.png --leaf2 leaf/willow_leaf_flip.png \
    --leaf-black 30
```

## 小苗（验拓扑 / 叶卡）

| 文件 | 规模 | 用途 |
|---|---|---|
| `willow.pbtxt` | 183 枝 / 442 卡 / 1 个 leaf type | 默认范本；扫管、叶卡 UV、gold 都用它 |
| `willow_two_types.pbtxt` | 215 枝 / 597 卡 / 2 个 leaf type | 双叶型（含 `bottom_right` 镜像手性）范本 |
| `willow_baked.glb` | 12 MB / 692 关节 / 干 40837 v / 叶 1768 v | 入库的**烘焙产物**，用来直接试「纯 glb 查看模式」（`--glb-only`），内嵌树皮+叶卡贴图 |

## 真树（验风力 / 实例化观感）—— 2026-10-27 入库

| 文件 | 树 | 规模 | 干顶点 / 叶顶点（烘焙后） |
|---|---|---|---|
| `pine_low.pbtxt` | 松（塔形、轮生枝） | 331 枝 / 1791 卡 | 42029 / 7164 |
| `oak_low.pbtxt` | 橡（宽冠阔叶） | 703 枝 / 3617 卡 | 26199 / 14468 |
| `fir_low.pbtxt` | 杉（塔形、下垂枝） | 977 枝 / 4747 卡 | 16915 / 18988 |

**为什么需要它们**：`willow` 只有 442 张卡，树冠又细又小 —— 风力/逐实例相位这类
「树冠越大越明显」的观感在小苗上根本看不出来（实测同一组参数：小柳树 lock vs 去相位
只有 35% 像素差异，橡树 48%，且肉眼的"整片一起晃"在橡树上才明显）。
做法：`--forest --wind-phase-span 0` vs `--wind-phase-span 2` 出两张同机位图对比。

**来源**（可复现）：由 `../configs/` 的生成配置经 `../generate_shape.py` 导出
（Blender + Sapling），导出时把 `export.per_seg` 调小以控制体积：

| 树 | 配置 | 导出的 `per_seg` |
|---|---|---|
| 松 | `configs/pine.pbtxt`（`small_pine`） | 8 |
| 橡 | `configs/oak.pbtxt`（`small_maple` + 球冠） | 2 |
| 杉 | `configs/fir.pbtxt`（`douglas_fir`） | 1 |

```bash
# 例：重生 oak（需要 Blender/Sapling 环境；per_seg 要与上表一致，否则规模/体积会变）
python3 tools/jpov/tools/vegetation_tool_v2/generate_shape.py \
    tools/jpov/tools/vegetation_tool_v2/configs/oak.pbtxt \
    --out tools/jpov/tools/vegetation_tool_v2/samples/oak_low.pbtxt
```

**烘焙产物不入库**（每棵 ~18 MB；`willow_baked.glb` 是唯一的例外，因为它是"纯 glb
查看模式"的唯一开箱入口）。自己烘一份：

```bash
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --shape shapes/oak_low.pbtxt \
    --bark bark/bark001 --leaf leaf/willow_leaf.png --leaf2 leaf/willow_leaf_flip.png \
    --leaf-black 30 --bake oak_low.glb
./jpov_vegetation_v2_viewer --glb oak_low.glb --forest     # 5×5 森林（逐实例风参数）
```

> bazel 侧：小苗在 `:sample_assets` filegroup（gold test 用），三棵真树单开
> `:sample_trees`（~4 MB，不想拖进只需要 willow 的测试 runfiles）。

# 范本：小柳树（samples/）

> 这个目录是一个**范本 / 示范**：既放**产物样例**（GLB + 出图），也记**过程动作**
> （这次是怎么做出来的），供后续「照着做」参考。
>
> 选柳树是因为它是**树里的困难题**（下垂细枝 + 密集叶），能同时演示形状可控性和
> 「质量卡在叶卡」这个核心认知。

---

## 1. 产物

| 文件 | 说明 |
|---|---|
| `willow_small_tex.glb` | **带纹理 GLB**，约 **8.8k tri / 2.1 MB**（小体积样例） |
| `willow_small_hero.png` | 出片（Blender Cycles 渲染） |
| `willow_wind_tex.glb` | **烘了风数据的版本**（2.4 MB，692 关节）——树干/叶子带 `TEXCOORD_1/2` + `jpov_wind_joints`，给 `jpov_wind_viewer` 当模特 |

> 体积刻意压小：17 MB 的满配柳树不适合入库当样例，这里是「小号」版本。

---

## 2. 输入（这一次是什么）

- **描述**：「一棵垂柳（困难题），带纹理，出一个**小体积**样例」
- **参考**：无（纯描述；有参考图时只用于选形态 / 选叶色 / 校对，不直接生成形状）

---

## 3. 过程动作（可复制）

```bash
cd tools/jpov/tools/vegetation

# ① 取 CC0 贴图（一次性）——ambientCG，全 CC0
bash fetch_textures.sh ./textures          # → textures/{leaf001,leafset024,bark001}/

# ② 生成「小柳树」带纹理 GLB
#    预设 = weeping_willow（垂柳）；用 --set 压小几何/体积
blender -b --python build_tree.py -- weeping_willow \
    --textured --textures ./textures --leaf-atlas leaf001 \
    --set scale=6 \
    --set curveRes=4,6,4,1 \
    --set branches=0,16,8,4 \
    --set leaves=10 --set leafScale=0.25 \
    --set seed=5 \
    --out samples/willow_small_tex.glb
# → [weeping_willow] trunk_polys=3942 leaf_polys=442 tris~8768 dims=(6.15,4.44,7.19)

# ③ 出片（大图，Cycles CPU，无需 GPU）
blender -b --python render_hero.py -- samples/willow_small_tex.glb \
    samples/willow_small_hero.png 900 600
```

---

## 4. 关键参数（怎么调）

| 参数 | 这次值 | 作用 |
|---|---|---|
| `--leaf-atlas` | `leaf001` | 叶卡贴图（单叶，一卡 2 叶）。`leafset024` = 9 叶 patch |
| `scale` | 6 | 整体尺寸（预设默认 15 = 大树） |
| `curveRes` | `4,6,4,1` | 每级枝的曲线分辨率（**降它就降面数**） |
| `branches` | `0,16,8,4` | 每级分枝数（**降它 = 稀疏**） |
| `leaves` | 10 | **每枝叶数**（不是总数！） |
| `leafScale` | 0.25 | 叶卡大小 |
| `seed` | 5 | 随机种子（同参同结果，可复现） |

> 参数用 `--set KEY=*** 透传 Sapling 任意字段；数组写逗号形式，如 `curveRes=4,6,4,1`。
> **加需求 = 加一次 `--set`**，不用改脚本。

---

## 5. 这次踩的坑（→ 都已在 `build_tree.py` 里修好）

1. **叶卡贴图需要 `leafShape=rect/hex`** 才有 UV（脚本已默认 `rect`）。
2. **Opacity 是灰度图（无 alpha）** → 必须接 **Color→Alpha**；接 `Alpha` 输出 = 恒 1 → **叶子变实心方块**（最大坑）。
3. **预设 `leafScaleX` 小（0.2）= 细条卡，像没叶子** → 脚本强制 `1.0`。
4. **`leaves` 是每枝叶数**，设成 200 会面数爆炸。
5. **老预设把数组写成标量**（如 `attractUp: -3.0`）→ 脚本按类型广播；算子没有的键按白名单过滤。

---

## 6. 渲染出片（两条路）

```bash
# (a) Blender Cycles 大图（本目录用的就是它）
blender -b --python render_hero.py -- samples/willow_small_tex.glb out.png 1600 1067

# (b) JPOV 引擎渲染（确认资产在引擎里能渲；需先 build viewer）
xvfb-run -a ./output/jpov_model_viewer/jpov_model_viewer \
    tools/jpov/tools/vegetation/samples/willow_small_tex.glb \
    --four_views --output_dir /tmp/veg_shots
```

---

## 7. 作为模板怎么用

- **要换树种**：把 `weeping_willow` 换成 §树 预设里任一个（`japanese_maple` / `small_pine` / …）。
- **要更密/更稀**：调 `leaves`（每枝叶数）。
- **要更轻/更重**：调 `curveRes` / `branches`。
- **要换叶形**：换 `--leaf-atlas`。
- **产物自检**：GLB 是**自包含**的（贴图内嵌），`tree`(Bark) + `leaves`(Leaf) 两条 mesh，
  Leaf 为 `alphaMode=MASK` + `doubleSided` —— 直接喂 JPOV 的 `branch_mesh` + `leaf_cards`。

---

## 8. 风版样例（`willow_wind_tex.glb`）

> 在上一版基础上多一个 `--bake-wind`。**完整烘焙说明见 `../README.md §6`**（顶点信息烘焙 SKILL）。

```bash
cd tools/jpov/tools/vegetation
blender -b --python build_tree.py -- weeping_willow \
    --textured --textures ./textures --leaf-atlas leaf001 \
    --bake-wind \
    --set scale=6 --set curveRes=4,6,4,1 --set branches=0,16,8,4 \
    --set leaves=10 --set leafScale=0.25 --set seed=5 \
    --out samples/willow_wind_tex.glb
```

自检（引擎里真在动才叫成）：

```bash
cd /james_pm/ai_infra_1
./tools/jpov/build_jpov_wind_viewer.sh
xvfb-run -a ./output/jpov_wind_viewer/jpov_wind_viewer --capture-wind /tmp/wind_shots
```

验收判据：关风帧 ≠ 开风帧；相邻秒也不同；**无撕裂**。小柳树像素变化量级 ≈ 4–5%。

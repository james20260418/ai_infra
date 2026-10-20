# JPOV 植被工具（Vegetation Tool）

> 位置：`tools/jpov/tools/vegetation/`
>
> **离线资产生成工具**：用外部参数化生成器出「形状」+ CC0 照片贴图出「外观」，
> 产出一棵**带纹理的 GLB**（枝干 + 叶卡），供 JPOV 直接加载。
>
> 一句话：**Blender 是出厂的机床，GLB 是机床的产品，JPOV 是用产品的下游。**

---

## 0. 为什么是这条路（定案理由）

经过预实验对比（Arbaro / Quaternius / Poly Haven / AI 单图生成 / 自研 L-System / 空间殖民），
选定 **Blender + Sapling Tree Gen**：

| 优点 | 说明 |
|---|---|
| **成本最低** | 参数化一键出树；不自研建模算法 |
| **形状可控** | 9 树种预设 × 10 形态预设 × seed/参数 = 组合爆炸的丰富度 |
| **格式天然对上引擎** | 产物拆成「枝干 mesh + 叶卡实例」，正好喂 JPOV 的 `branch_mesh` + `leaf_cards` |
| **全 CC0 贴图** | 免署名、可商用，直接能出货 |
| **效果 OK** | 当前 ≈ 中档风格化（远景/中景够用），质量杠杆在叶卡质量 |

> ⚠️ **质量瓶颈不在骨架，在「叶卡 + atlas」**：单叶卡 ≈ 风格化；要往上冲需换「叶簇卡」。

---

## 0.5 输入与迭代（这是个「活」工具）

这个工具**真正的输入不是一份固定规格，而是「人 ↔ agent 的来回交流」**：

```
需求（一句话 / 参考图） → 试拍 → 看图 / 量数（tris、尺寸、像素 diff） → 改 → 再试 ……
```

- 脚本（`build_tree.py` / `render_hero.py`）只是**那只手**；**判断与翻译在对话里**（选哪个预设、往哪调参、换哪张 atlas）。
- 因此本工具**刻意保持薄**：一个可读的 build 脚本 + 一个渲染脚本，好改、好迭代。
- 「描述 → 预设 / 参数」的映射**没有锁死**，随实际需求演进（**可能改版**）。

---

## 1. 目录内容

```
tools/jpov/tools/vegetation/
├── README.md            # 本文档（依赖 + 工作流 + 渲染步骤）
├── build_tree.py        # 【核心】Blender headless 脚本：树种预设 → 带纹理 GLB
├── wind_bake.py         # 【核心·风】顶点风信息烘焙（(joint_id, phase) + 枢轴关节表）
├── render_hero.py       # Blender Cycles 出片（大图 hero shot）
├── fetch_textures.sh    # 下载 CC0 贴图（ambientCG）
└── samples/             # 【范本】产物样例 + 过程动作记录（照着做用），见 samples/README.md
```

---

## 2. 依赖（每步需要什么）

### 2.1 软件

| 软件 | 用途 | 装法 | 备注 |
|---|---|---|---|
| **Blender 4.0.2** | 出资产（形状 + 贴图 + 导 GLB） | `apt-get install -y blender` 或官方 portable 包 | **dev-time 工具，非 JPOV 运行时/编译依赖** |
| **Sapling Tree Gen** | 参数化树生成器 | Blender **自带插件**（`scripts/addons/add_curve_sapling/`） | 无需额外安装；预设在其 `presets/` 子目录 |
| `xvfb` | JPOV viewer 无显示器渲染 | `apt-get install -y xvfb` | 本容器无 `$DISPLAY`，须 `xvfb-run -a` |
| `imagemagick` | 拼图/montage（可选） | `apt-get install -y imagemagick` | 只做 gallery/对比图时用 |
| `curl` + `unzip` | 下载/解压贴图 | 系统自带 | |
| Bazel | 构建 JPOV（渲染那步） | 见仓库 `WORKSPACE` | |

> **验证 Blender 可用**：`blender --version` → `Blender 4.0.2`。
> 检查插件：`blender -b --python-expr "import bpy; bpy.ops.preferences.addon_enable(module='add_curve_sapling'); print('ok')"`

### 2.2 贴图网站（全 CC0）

| 网站 | 取什么 | 例（本工具默认） |
|---|---|---|
| **https://ambientcg.com** | 叶 atlas / 树皮（**带 Opacity 遮罩**） | `Leaf001`（单叶，一卡 2 叶）、`LeafSet024`（9 叶 patch）、`Bark001` |
| https://polyhaven.com | 树皮 / 植被 | 树皮平铺图 |
| https://www.freecutout.com | 单叶 PNG（带 alpha） | 各树种单片叶 |

> CC0 = 免署名 + 可商用。下载方式见 §3.1（`fetch_textures.sh` 已内置 ambientCG 直链）。

---

## 3. 工作流（五步）

### 步骤 0（一次性）：装 Blender

```bash
apt-get install -y blender        # 或解压官方 portable tar.xz
blender --version                 # 期望：Blender 4.0.2
```

### 步骤 1：下载 CC0 贴图

```bash
cd tools/jpov/tools/vegetation
bash fetch_textures.sh ./textures
# → textures/{leaf001,leafset024,bark001}/
```

### 步骤 2：生成树 → **带纹理的 GLB**

```bash
# 单棵（默认不贴图；加 --textured 出带纹理版）
blender -b --python build_tree.py -- japanese_maple --textured \
    --textures ./textures --leaf-atlas leafset024 --set leaves=14

# 垂柳困难题（带纹理，大图用）
blender -b --python build_tree.py -- weeping_willow --textured \
    --textures ./textures --leaf-atlas leaf001 \
    --set leaves=60 --set leafScale=0.3 --out-dir ./out
```

**产物**：`out/<species>_tex.glb` —— **带纹理的 GLB**（见 §4 格式）。
脚本末尾会打印 `tris~N / dims=(x,y,z)`。

> `--set KEY=VALUE` 可覆盖 Sapling 任意参数；数组写成逗号形式，如 `--set curveRes=6,3,2,1`。
> 叶子密度是 `leaves`（**每枝叶数**，不是总数！设太大面数爆炸）。

### 步骤 2.5（要风吹就加）：烘焙顶点信息

**只有加了 `--bake-wind`，树才会吹风**；不加 = 静态树（风开关打开也不动）。
它把「每顶点的关节下标 + 相位 + 枢轴关节表」**烘进同一个 GLB**：

```bash
blender -b --python build_tree.py -- weeping_willow \
    --textured --textures ./textures --leaf-atlas leaf001 \
    --bake-wind \
    --out samples/willow_wind_tex.glb
```

完整原理 / 产物格式 / 验收 / 坑 → **§6 顶点信息烘焙（SKILL）**。

### 步骤 3：用 veg tool 渲染出片

#### 3a. Blender Cycles 大图（hero shot，无需 GPU）

```bash
blender -b --python render_hero.py -- out/weeping_willow_tex.glb \
    /tmp/willow_hero.png 1600 1067
```

- 输出指定 PNG（宽 高 可选）。
- Cycles **CPU** 渲染（本容器 EEVEE headless 无法初始化 EGL，必须用 Cycles）。
- 默认已关降噪（OpenImageDenoise 不一定编译进包）。

#### 3b. JPOV 引擎渲染（确认资产在引擎里能渲）

```bash
cd /james_pm/ai_infra_1
xvfb-run -a ./output/jpov_model_viewer/jpov_model_viewer \
    tools/jpov/tools/vegetation/out/weeping_willow_tex.glb \
    --four_views --output_dir /tmp/veg_shots
# → /tmp/veg_shots/<name>_{front,perspective,left,up}.png
```

- JPOV 引擎**已支持**：GLB 载入 + cutout（`AlphaMode::kMask`）+ 双面 + instancing。
- viewer 需先构建：`./tools/jpov/build_jpov_model_viewer.sh`。
- 无显示器环境须 `xvfb-run -a` 包一层。

---

## 4. 产出格式

**是带纹理的 GLB**（`.glb`，自包含）。内部结构（两条 mesh）：

```
tree    -> POSITION/NORMAL/TEXCOORD_0 -> 材质 Bark   # 枝干（管道 mesh）
leaves  -> POSITION/NORMAL/TEXCOORD_0 -> 材质 Leaf   # 叶卡实例
```

**若用了 `--bake-wind`，每条 mesh 会多一层 `TEXCOORD_1`（UV 层名 `"Wind"`），
trunk 节点还会多一份关节表：**

| 产物 | 位置 | 语义 |
|---|---|---|
| `TEXCOORD_1` | UV 层 `"Wind"` | `(joint_id, phase)`：顶点所属刚性链在关节表里的下标 + 连续相位场（秒） |
| `jpov_wind_joints` | **trunk 节点 extras** | 关节表，每关节 **7 float**：`pivot.xyz, parent, level, phase, soft` |

材质：
- `Leaf`：`alphaMode=MASK`（cutout）+ `doubleSided=true`，baseColor = 叶 atlas（颜色 + alpha 由 Opacity 图烘进 alpha 通道）。
- `Bark`：baseColor = 树皮贴图。
- 贴图**内嵌**进 GLB（不依赖外部文件）。

这正是「**拓扑枝干 + 片片叶子（叶卡）**」，与 Poly Haven 扫描树的拆解格式一致 —— 都能直接喂
JPOV 的 `branch_mesh` + `leaf_cards`。

---

## 5. 树种预设（Sapling 自带 9 个）

`callistemon` `douglas_fir` `japanese_maple` `quaking_aspen` `small_maple`
`small_pine` `weeping_willow` `white_birch` `willow`

> 预设文件：`<blender>/scripts/addons/add_curve_sapling/presets/*.py`
> 另可用 `--set shape=<0..10>` 调 10 档整体形态。

---

## 6. 顶点信息烘焙（SKILL：让树能吹风）

> 一句话：**风数据是「烘」进 GLB 的，不是引擎猜的。**
> 不烘 → 树是死的；烘错 → 树会**撕裂 / 拉长 / 整棵平移**。
> 两条渲染路径都吹风：**object3d 路径**（逐物体参数）与**静态实例路径**
> （同 mesh 摆 N 份，`InstancedObjectRenderer`；逐实例 `blow_away_ratio`）。

### 6.1 烘焙什么（两样东西，全在 GLB 里）

| 产物 | 存放位置 | 语义 |
|---|---|---|
| `TEXCOORD_1` | 每条 mesh 的 UV 层 `"Wind"` | `(joint_id, phase)`：`joint_id` = 顶点所属刚性链在关节表中的下标；`phase` = 连续低频相位场（秒） |
| `jpov_wind_joints` | **node extras**（trunk 节点） | 关节表，每关节 **7 float**：`pivot.xyz, parent, level, phase, soft` |

- `parent = -1` 是根关节；`level` = 距根的关节数；`soft` = 朝向继承度（`0`=硬/全继承父朝向，`1`=软/只随自己的角）。
- 关节表**随资产走**；引擎里 `tree` / `leaves` 两个 primitive 共享同一张表。
- **枢轴坐标已转成 glTF 的 Y-up**（Blender Z-up → `(x, z, -y)`），**不要**手工再转。
- 引擎字段：`MeshData::wind_link`（每顶点 2 个数）+ `MeshData::wind_joints`（关节表），见 `interface/mesh.h`。
- **顶点标签算法（2026-10-19）**：表面**测地 Voronoi** —— 种子只取关节自己所属样条上的
  顶点、按连通块多数票约束，标签沿网格边传播。旧的「空间最近关节」会让枝关节泄漏到
  几米外的干顶点（一条 1.35m 的树干三角形，一个角被标成枝关节）。

### 6.2 怎么烘（造树时顺手烘）

`--bake-wind` **必须与造树同一次运行** —— 骨架采样发生在 `curve → mesh` 转换**之前**，事后再补采不到。

```bash
cd tools/jpov/tools/vegetation

blender -b --python build_tree.py -- weeping_willow \
    --textured --textures ./textures --leaf-atlas leaf001 \
    --bake-wind \
    --set scale=6 --set curveRes=4,6,4,1 --set branches=0,16,8,4 \
    --set leaves=10 --set leafScale=0.25 --set seed=5 \
    --out samples/willow_wind_tex.glb
```

除 `--bake-wind` 外，参数与「不烘」完全一样。
⚠️ **改任何形状参数（species / seed / scale / curveRes / branches / leaves）都必须重烘**：
属性是按这一版骨架算的，几何变了属性就对不上。

正常输出（小柳树参考值；绝对值会随参数变，**看比例与链长**）：

```
[wind_bake] splines=183 max_hd=... levels=... joints=692
[wind_bake] max_chain_len=14
[wind_bake] 柳条样条=103/183  关节 s>0.05: .../692
[wind_bake] 树皮顶点覆盖：s>0.25 占 25% ...
[wind_bake] 测地 Voronoi 标签：<顶点数> 顶点 / <关节数> 关节
[wind_bake] 同位置顶点链归一：<n> 个顶点
[wind_bake] 跨枝链面(祖先口径)：<n> / <faces>
[wind_bake] 跨样条面：<n> / <faces>      # ← 真·跨枝；正常应尽量小
```

### 6.4 排查工具（`jpov_vegetation_viewer`）

```bash
# 导出「当前风形态」的网格 + 逐顶点 joint/L + 逐三角形面积比（CPU 同公式，GL-free）
jpov_vegetation_viewer --glb plants/willow_wind_geo_tex.glb   --save-mesh /tmp/vexp/tree --blow 0.25 --blow-exponent 2 --wind-time 0.35
# → tree.obj / tree_verts.csv / tree_tris.csv
```

`willow_wind_geo_tex.glb` 是用新的测地标签重烘的柳树样例（几何不变）。

### 6.3 验收（在引擎里确认它真在动）

```bash
cd /james_pm/ai_infra_2
./tools/jpov/build_jpov_vegetation_viewer.sh

# headless：出「关风参照帧 + 开风逐秒帧」
xvfb-run -a ./output/jpov_vegetation_viewer/jpov_vegetation_viewer \
    --glb tools/jpov/tools/vegetation/samples/willow_wind_tex.glb \
    --capture-wind /tmp/wind_shots
```

- 产物：`wind_off.png` + `wind_t000/025/050/075/100.png`。
- 判据：**关风帧 vs 开风帧差异非零；相邻开风帧也不同；任何一帧都不该出现撕裂/宽薄片**。
  小柳树上像素变化量级 ≈ **3–5%**。
- 交互面板：风开关 / 风向 / pivot 振幅与频率 / 平动湍流幅度与频率 / 湍流随机性。

### 6.4 烘焙正确性红线（都是踩过的坑）

1. **必须 `curve → mesh` 之前采样骨架** —— `build_tree.py --bake-wind` 已处理；别对已导出的 GLB 后补。
2. **`phase` 必须空间连续**（连续低频场），**不能**用 per-vertex hash 或 per-branch 常量 → 否则接缝必撕裂。
3. **叶卡整卡取一个值**，且从**最近实体顶点**继承 → 卡不会被剪切、不会脱离所挂细枝。
4. **同位置重复顶点必须归一到同一根链**（UV 缝/焊点），否则会拉出宽薄片；`wind_bake` 自动按「更靠根」裁决并打印修复计数。
5. **关节按枝粗细分级抽稀**：粗枝少关节、细枝密关节；链过长会让枢轴误差放大 —— 看 `max_chain_len`，**>20 要警惕**。
6. **有 `TEXCOORD_1`、`jpov_wind_joints` 缺失/长度不是 7 的倍数** → loader 告警并清空风数据（树退回静止）。看到告警 = 资产烘坏了，**重烘，别手改 GLB**。
7. **`kWind` 与布料 `kRelax` 共用 loc6、互斥** —— 同一 mesh 不能既是植被又是布料。
8. 体积会变大：小柳树 **2.1 MB → 2.3 MB**（+1 层 UV + 692×7 float），属正常。

### 6.5 最小自检清单（给 AI / 自动化）

- [ ] GLB 有 `tree` / `leaves` 两条 mesh，且**都**带 `TEXCOORD_1`
- [ ] trunk 节点 `extras.jpov_wind_joints` 存在，长度 `% 7 == 0`
- [ ] 根关节 `parent == -1`；所有 `parent < 自身下标`（无环）
- [ ] `--capture-wind` 出图：关风 ≠ 开风；相邻开风帧也不同；无撕裂

## 7. 已知坑（踩过，务必复用）

1. **叶卡贴图 UV 只在 `leafShape ∈ {rect, hex}` 生成** → 贴图必须 `rect`（脚本已默认）。
2. **Opacity 是灰度 JPG（无 alpha 通道）** → 必须把它接 Principled 的 **Color→Alpha**（脚本 `make_material` 已处理）。
   接 `Alpha` 输出 = 恒 1.0 → **叶子变实心方块**（本工具修得的最大坑）。
3. **`leafScaleX` 小**（预设常 0.2）= 细条卡**看起来像没叶子** → 要 `1.0`（脚本已强制）。
4. **`leaves` = 每枝叶数**，设成 200 会 79 万面爆炸。
5. **老预设数组写成标量**（如 `attractUp: -3.0`）→ 脚本按 `array_length` 广播；预设里算子没有的键按 `VALID` 过滤。
6. **本容器 EEVEE headless 不可用**（EGL 无 GPU）→ 用 Cycles。

---

## 8. GPL / 依赖声明（重要）

**Blender 是 GPL 的独立应用（不是库）**，本工具只在**离线出资产**时用它：

- JPOV 运行时**不链接、不依赖** Blender；唯一耦合是 **GLB 文件格式**。
- **只用其产出（GLB）** → 资产不受 GPL 传染，可随产品出货。
- ⚠️ **别把 Blender 打进产品分发包**。若做产品镜像，用 **multi-stage** 把 Blender 留在
  builder 阶段，final 阶段只放 JPOV 产物 + 非 GPL 运行时。
- Blender 装在**系统/dev 环境**里即可（跟 `gcc`/`make` 一样都是 dev 工具）。

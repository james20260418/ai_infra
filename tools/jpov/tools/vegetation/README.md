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

## 3. 工作流（四步）

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

## 6. 已知坑（踩过，务必复用）

1. **叶卡贴图 UV 只在 `leafShape ∈ {rect, hex}` 生成** → 贴图必须 `rect`（脚本已默认）。
2. **Opacity 是灰度 JPG（无 alpha 通道）** → 必须把它接 Principled 的 **Color→Alpha**（脚本 `make_material` 已处理）。
   接 `Alpha` 输出 = 恒 1.0 → **叶子变实心方块**（本工具修得的最大坑）。
3. **`leafScaleX` 小**（预设常 0.2）= 细条卡**看起来像没叶子** → 要 `1.0`（脚本已强制）。
4. **`leaves` = 每枝叶数**，设成 200 会 79 万面爆炸。
5. **老预设数组写成标量**（如 `attractUp: -3.0`）→ 脚本按 `array_length` 广播；预设里算子没有的键按 `VALID` 过滤。
6. **本容器 EEVEE headless 不可用**（EGL 无 GPU）→ 用 Cycles。

---

## 7. GPL / 依赖声明（重要）

**Blender 是 GPL 的独立应用（不是库）**，本工具只在**离线出资产**时用它：

- JPOV 运行时**不链接、不依赖** Blender；唯一耦合是 **GLB 文件格式**。
- **只用其产出（GLB）** → 资产不受 GPL 传染，可随产品出货。
- ⚠️ **别把 Blender 打进产品分发包**。若做产品镜像，用 **multi-stage** 把 Blender 留在
  builder 阶段，final 阶段只放 JPOV 产物 + 非 GPL 运行时。
- Blender 装在**系统/dev 环境**里即可（跟 `gcc`/`make` 一样都是 dev 工具）。

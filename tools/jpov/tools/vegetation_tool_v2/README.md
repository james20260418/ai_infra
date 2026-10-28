# vegetation_tool_v2 —— 植物**组合查看器**

把 `shape`（拓扑）+ `bark`（树皮材质）+ `leaf`（叶卡素材）组合成植物并渲染出来。
**人和 AI 都用它做资产验收**：三样给几样就渲染几样。

**两个状态**（右下「烘焙 glb」面板一键切换）：

| 状态 | 在画什么 | 用途 |
|---|---|---|
| **原材料态** | 现搭的 shape/bark/leaf（逐枝扫管 + 叶卡**曲面**） | 看资产：拓扑、叶卡曲面、噪声后的枝干；叶卡可用 debug 绿卡看对齐 |
| **glb 展示态** | 刚烘焙出来的 `.glb`（树干 + 叶卡两个 primitive，带风链数据） | 看最终资产：树皮贴图、叶卡 alpha、**风**（左下「风调试」面板） |

烘焙产物是一份**自洽 .glb**：几何 + 内嵌贴图（树皮 albedo/normal/ORM、叶卡 albedo/normal）
+ 逐顶点 `(joint_id, phase)`（glTF `TEXCOORD_1`）+ 关节表（`node.extras.jpov_wind_joints`，
每关节 `pivot.xyz/parent/level/phase/soft`）。引擎 `LoadGltf` 读回即可直接吹风。

```
shape.vegshape ─┐
bark/        ──┼─→ jpov_vegetation_v2_viewer ─→ 交互窗口 / 三张 capture PNG
leaf.png     ──┘
```

| 输入 | 必需 | 省略时 |
|---|---|---|
| `shape` = **二进制资产**（`.vegshape`，ShapeProto 的 protobuf 二进制） | 有默认值 | 用打包目录里的 `shapes/willow.vegshape`；要看文本用 `shape_convert` |
| `bark` = `albedo.png` + `normal.png`(GL) + `roughness.png` | 否 | 树干按 level 上棕色 |
| `leaf` = 一张黑底叶片图（整图 = 一格） | 否 | 只画 debug 绿卡（纯拓扑视图） |

格式约定（单位/上方向/anchor/UV/校验）见 **[docs/vegetation_asset_format.md](docs/vegetation_asset_format.md)**；
样例素材的来源与约定见 **[assets/README.md](assets/README.md)**。

## 30 秒上手

```bash
cd tools/jpov && ./build_jpov_vegetation_v2_viewer.sh
cd output/jpov_vegetation_v2_viewer

./jpov_vegetation_v2_viewer                       # 只渲染拓扑（默认范本）
./jpov_vegetation_v2_viewer --leaf leaf/willow_leaf.png --leaf-black 30 \
    --bark bark/bark001                           # 完整组合（交互窗口，需 DISPLAY/WSLg）
xvfb-run -a ./jpov_vegetation_v2_viewer --capture shots/full \
    --leaf leaf/willow_leaf.png --leaf-black 30 --bark bark/bark001   # 无窗口出图

# 烘焙一份 glb（不开窗口，CI 可用）→ 再直接打开它看风
./jpov_vegetation_v2_viewer --bake output/veg.glb \
    --shape shapes/willow.vegshape --leaf leaf/willow_leaf.png --bark bark/bark001
./jpov_vegetation_v2_viewer --glb output/veg.glb            # 交互：左下角风调试面板

# 纯 glb 查看模式：**没有 shape 也能开**（别人发来一个 glb，只想看看它长什么样）
cd anywhere && ./jpov_vegetation_v2_viewer --glb some.glb
# 在打包目录里（shapes/ 存在）想强制纯查看：加 --glb-only
./jpov_vegetation_v2_viewer --glb some.glb --glb-only
```

**纯 glb 查看模式**（2026-10-22 加）：`--glb-only`，或给了 `--glb` 且 **shape 文件找不到**
（默认的 `shapes/willow.vegshape` 也不在 cwd）时只开 glb 展示态 —— 有地面、有风、有风调试面板，**没有**原材料态：面板上不会出现
「回到原材料态」，也不能烘焙（两者都需要 shape）。相机默认按 glb 包围盒自动取景。
渲染结果与「shape + glb」逐像素一致（同一机位实测 0 差异），只是少了回退能力。

**不用自己烘也能试**：仓库里带了一份烘焙产物
`samples/willow_baked.glb`（12.5 MB，692 关节 / 树干 40837 v / 叶卡 3978 v（442 张 × 9 顶点/弯卡），
内嵌树皮+叶卡贴图）。

```bash
cd tools/jpov && ./build_jpov_vegetation_v2_viewer.sh
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --glb samples/willow_baked.glb --glb-only
```

它就是用本目录的素材、按面板默认参数烘出来的：

```bash
./jpov_vegetation_v2_viewer --bake output/veg.glb \
    --shape shapes/willow.vegshape --leaf leaf/willow_leaf.png --leaf-black 30 \
    --bark bark/bark001 --normal-scale 0.75
```

> 两份产物应当**逐字节相同**（md5 `93b0ac1ffe756a4493874593875a7efa`）。
> 若你的 md5 不同：先确认上面命令的参数一字不差（风/关节参数走面板默认值），
> 再确认 `AI_INFRA` 版本一致 —— 烘焙结果对几何与参数都很敏感。

- 打包目录里必须有 `fonts/` 才能画文字；**没有字体不会崩**，只是不画 HUD/标签。
- `--leaf-black`：黑底素材的「近乎黑 = 透明」阈值，软过档是 `[0.5t, 1.5t]`。
  入库样例背景是 `≈10~13` 的灰噪、内容 `≥44` ⇒ 给 **30**（`[15,45]`）；默认 12 会把灰噪留成半透明。

## 从配置生成 shape（Blender 侧）

上文那些资产是用 Blender + Sapling 离线长出来的。两条路，**产物等价**：

| 路径 | 入口 | 说明 | fir（4100 枝） |
|---|---|---|---|
| **C++（推荐）** | `gen/shape_gen` | Blender 只把曲线/叶卡网格倒成 `.dump`；采样、父枝、叶卡姿态、拓扑序、写 `.vegshape` 全在 C++ | **1.5 s** |
| Python（对照基准） | `generate_shape.py` | 原实现，留作对拍/回归基准 | 132 s |

```bash
# C++：配置 -> shape.vegshape（Blender 只负责倒数据）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_gen \
    --config tools/jpov/tools/vegetation_tool_v2/configs/pine.pbtxt \
    --out output/veg_shapes/pine.vegshape

# 只看配置有没有写对（不跑 Blender）
bazel run //tools/jpov/tools/vegetation_tool_v2/gen:shape_gen -- --help
```

| 层 | 位置 | 说明 |
|---|---|---|
| 配置（**输入**） | `configs/*.pbtxt`，schema 见 `proto/generate.proto` | 每个字段都写了含义/单位/效果预期；没写的字段 = 用预设自带值（proto2 optional 的存在性） |
| C++ 入口 | `gen/shape_gen.cc` | 读配置 → 调 Blender → 建 shape → 自检（唯一根 + 拓扑序）→ 写 `.vegshape`（`--text` 可写文本） |
| C++ 几何 | `gen/shape_builder.cc` | 采样 / 父枝（`tools/util` 的稀疏栅格最近邻）/ 叶卡姿态 / anchor 合同 |
| Blender 侧 | `dump_sapling_curve.py` | **只做序列化**：建树 + 把曲线控制点和叶卡网格倒成 `.dump`，不做任何几何决策 |

- **Blender 不在仓库里**：它是镜像里的开发期工具（`/usr/bin/blender` 4.0.2），`--blender` 可改路径。
- 单测：`bazel test //tools/jpov/tools/vegetation_tool_v2/gen:all`
  （dump 解析的容错边界 + 合成 dump 的父枝/层数/环数/叶卡姿态/拓扑序/anchor）。
- 与 Python 产物的差异：几何逐行一致（实测 93424 行里 0 处结构不同，最大数值差 1e-6 量级），
  唯一例外是「卡片正好骑在枝上」时根/梢方向在数学上无定义，约 1% 的卡在两种浮点精度下取反。
  另外 C++ 侧会多写一段 `leaf_types.bend`（叶面弯曲，见 docs §4.5）——Python 基准没实现它，
  所以配置里写了 `export.leaf_bend_*` 时两边产物在这一段上有差异，其余行不受影响。
- 生成配置的取舍与已知缺陷见 [`configs/README.md`](configs/README.md)。

## 工具总览

| 工具 | 输入 | 输出 | 职责 |
|---|---|---|---|
| `gen/shape_gen` | 生成配置（`configs/*.pbtxt`） | `shape.vegshape` | 建**拓扑**：调 Blender 倒数据 → C++ 采样/父枝/叶卡姿态/拓扑序 |
| `leaf_card/jpov_leaf_card` | 生图的叶子（黑底） | 合规叶卡（+ 镜像版） | **叶卡**：根点识别 / 旋转对齐 / 裁到锚点 |
| `make_generated_bark.sh` | 生图的树皮 | `albedo/normal/roughness.png` | **树皮**：无缝化 + 派生法线/粗糙度 |
| `jpov_vegetation_v2_viewer` | shape + bark + leaf | 窗口 / `--capture` 出图 | **验收**：人眼 + gold |

四者只通过**文件**耦合，没有隐藏状态：任何一步都能单独重跑、单独替换。

---

## 0. 资产格式：`.vegshape`（二进制）+ 转换工具

**约定（2026-10-26 定）：植被拓扑资产一律存二进制 `.vegshape`**（ShapeProto 的 protobuf
wire format，`shape.proto` 里的 `ShapeProto`）。文本 `.pbtxt` **只用于人工查看 / diff / review**，
仓库里不放文本资产。

理由很实际：三棵树的文本是 1.2~1.4 MB，二进制后 0.5~0.7 MB（**约 1/3**），
而且加载不用解析文本。

**文本是「可逆的一半」，不是「大概能看的另一半」**：`FormatShapeText` 写每个 double 都用
**最短的可逆表示**（从 1 位有效数字往上试，第一个「读回来仍是同一个 double」的写法），
所以二进制 → 文本 → 二进制**逐字节相同**（形状测试 `TestShapeTextIsLossless` 钉住，含 `-0`
的符号）。第一版偷懒沿用 Python 的 `%.6g`（6 位有效数字），实测 `0.25691019529015585`
被写成 `0.256910`、读回来是另一个 double —— 那就是「diff 里看到的数和资产里的数不是一个数」。
常见的短数字（`0.25691` / `1.5` / `-0.08`）仍然短，只有真需要更多位才变长。

格式**由扩展名决定**：`.vegshape` → 二进制，其它（`.pbtxt`/`.txt`）→ 文本。一条规则，不猜内容。

```bash
# 二进制 → 文本（看 / diff / review）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_convert \
    --in tools/jpov/tools/vegetation_tool_v2/samples/oak_low.vegshape --out /tmp/oak_low.pbtxt

# 文本 → 二进制（改完转回去；顺便打印枝/卡/顶点/三角形）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_convert \
    --in /tmp/oak_low.pbtxt --out /tmp/oak_low.vegshape --stats
```

- 转换一定过一遍 `LoadShapeFile` 的校验（拓扑、半径、anchor 合同），坏资产转不出去；
- 8 份入库资产实测「`vegshape → pbtxt → vegshape` **逐字节相同**」（数值一点没丢）；
  六个 `shape_gen` 档位（`{pine,oak,fir}_{low,game}`）同样可逆。
- 两个旧 Python 范本（`willow` / `willow_two_types`）的文本与 `shape_gen` 的写法差在缩进
  （卡片字段 4→2 空格），数值一致；把它们的文本再转回二进制与入库 `.vegshape` **0 字节差异**。
- `--format text|binary` 可强制格式，但正常不需要（看扩展名就够）。

## 1. `shape_gen` —— 生成拓扑

```bash
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_gen \
    --config tools/jpov/tools/vegetation_tool_v2/configs/pine.pbtxt \
    --out output/veg_shapes/pine.vegshape
```

| 参数 | 说明 |
|---|---|
| `--config` | 生成配置（schema 见 `proto/generate.proto`，每个字段都写了含义/单位/效果） |
| `--out` | 产物路径；临时 `.dump` 默认写 `<out>.dump` 并在成功后删除 |
| `--blender` | blender 可执行文件（默认 PATH 里的；镜像自带 4.0.2） |
| `--dump <path>` / `--keep-dump` | 保留中间 dump（排查 Blender 侧问题时用） |
| `--per-seg <n>` | 覆盖配置里的 `export.per_seg`（每段贝塞尔的采样点数） |
| `--ring-count 8,6,4` | 临时覆盖每级枝的**环顶点数**（正式的写在配置 `export.ring_count`；命令行优先，试参数用） |

输出末尾一行是摘要：`枝数 / 卡数 / 镜像卡比例 / 体积 / 耗时`。
**失败即退出码非 0**：配置写错（带行号）、Blender 失败、产物拓扑序不合法，都不会落盘半成品。

耗时（含 Blender 启动）：几百枝 ~0.6 s，4100 枝 ~1.5 s。

---

## 2. `jpov_leaf_card` —— 叶卡归一化

```bash
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/leaf_card/jpov_leaf_card \
    raw_leaf.png out/willow_leaf.png \
    --anchor bottom_left --also-flip out/willow_leaf_flip.png
```

| 参数 | 说明 |
|---|---|
| `--anchor` | 根部要落在格子的哪个角/边：`bottom_left` / `bottom_center` / `bottom_right` / `top_center` |
| `--also-flip` | 额外输出左右翻转版（镜像手性 = 第二个 leaf type；`anchor_mode` 会自动换成 `bottom_left ↔ bottom_right`） |

它做四件事：**切内容 → 找叶柄（最细的一端）→ 旋转到"从锚点指向格内"→ 裁到根点正好压住锚点**。
输出会打印：

```
叶柄方向 -62.2°，旋转 17.2° → anchor=bottom_left
根点偏差 du=+0.000 dv=+0.000      ← 必须是 0，否则叶子会悬空
leaf_aspect = 1.180               ← 直接填进生成配置的 export.leaf_aspect
⚠ 灰区像素占内容 3.8%             ← 生图没守约定（叶子太暗/边缘太糊）
```

**生图合同**（数字唯一定义在 [`leaf_card/leaf_card_spec.h`](leaf_card/leaf_card_spec.h)）：

| 项 | 要求 | 判据 |
|---|---|---|
| 背景 | 纯黑 | `max(r,g,b) <= 16` |
| 内容 | 叶子颜色**避开黑色** | `max(r,g,b) >= 48` |

判据统一用 **max 通道**（不是求和/亮度加权），因为 `leaf_texture` 的黑底→alpha 就是这么判的。

---

## 3. `make_generated_bark.sh` —— 树皮三件套

```bash
./tools/jpov/tools/vegetation_tool_v2/make_generated_bark.sh raw_bark.png out/bark/gen001 1.5 15
```

| 参数 | 说明 |
|---|---|
| 第 1 个 | 生图树皮（任意尺寸） |
| 第 2 个 | 输出目录（viewer 用 `--bark <dir>` 直接吃） |
| 第 3 个 | 法线强度，默认 1.5（与 viewer `--normal-scale` 默认一致） |
| 第 4 个 | 无缝化边缘带宽占短边的**百分比**（0~50 的整数），默认 15 |

顺序是 **seamless（albedo）→ normal（Sobel）→ roughness（1 − 亮度）**。
为什么不反过来：法线必须从**已经无缝**的 albedo 算，否则接缝处的法线也是错的。

---

## 4. `jpov_vegetation_v2_viewer` —— 验收

```bash
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --shape ../veg_shapes/pine.vegshape \
    --bark /tmp/bark_gen --leaf /tmp/leaf.png --leaf2 /tmp/leaf_flip.png --leaf-black 32
```

`--leaf-black` 的默认值就是 **32**（= `kLeafBlackThreshold`）；只有换素材约定时才需要显式传。

---

## 技能：从一段描述到一棵树

> 这一步不写死脚本 —— 它本来就是"人/agent 判断 → 调工具 → 看图 → 再调"的循环，
> 每个环节的命令和判据都在上面。下面是一份可以直接照做的清单。

**① 选预设、写配置**（`configs/*.pbtxt` 抄最像的一份）
### 枝条噪声（`noise`）—— 让树别那么整齐

`configs/*.pbtxt` 里的 `noise` 块对应 `proto/generate.proto` 的 `ShapeNoise`：

| 字段 | 在树上看到的效果 | 建议值 |
|---|---|---|
| `size_jitter` | 每根枝整体放大/缩小（长度与半径乘同一个倍数），同级的枝不再一样长一样粗 | 0.1~0.2 |
| `bend_longitudinal` | 中段朝「上下/前后」鼓出：竖直主干弓背，水平侧枝上翘/下垂 | 主干 0.03~0.08 / 侧枝 0.05~0.15 |
| `bend_lateral` | 中段朝侧向扭（俯视图里的左右摆动，正视图几乎看不出） | 0.02~0.10 |

三个字段都是**按枝级**的数组（下标 0 = 主干），写 0 或数组写短 = 该级不抖。
噪声结果直接烘进 shape 的采样点（位置/半径），**资产里没有噪声参数**；
同 `tree.seed` + 同参数 ⇒ 逐字节相同的树。

⚠️ 只有 C++ 版（`gen/shape_gen`）实现 `noise`、`export.leaf_bend_*` 与 `export.ring_count`；
`generate_shape.py` 是历史对照基准 —— 这三块它**全部忽略**（不报错，环数写死 16/12/8、
不带弯曲、不加噪声），所以拿它跑 `configs/low|game/` 会得到一棵完全不同的树，别拿它出正式资产。

### 顶点预算 & 低配档（`configs/low/`）

一棵树的顶点 = **枝管**（采样点数 × 环顶点数）+ **叶卡**（平面卡 4 / 弯曲卡 9 顶点，
见 `sweep.h` 的 `kBentCardSegments`）。实测口径：

| 树 | 高配（`configs/`） | 低配（`configs/low/`） |
|---|---|---|
| pine | 管 69k + 卡 48k = **117k** | 管 35k + 卡 16k = **51k** |
| oak | 管 152k + 卡 193k = **345k** | 管 21k + 卡 33k = **54k** |
| fir | 管 606k + 卡 114k = **720k** | 管 11k + 卡 46k = **57k** |

低配档只动四个旋钮，树形（预设、seed、分枝角度、噪声）完全不变：

| 旋钮 | 作用 | pine | oak | fir |
|---|---|---|---|---|
| `leaves` | 每根末级枝的叶片上限（卡数是顶点大头） | 55 → 18 | 200 → 36 | 12 → 22 |
| `leafScale` | 叶片尺寸（用更少更大的叶子补覆盖率） | 0.42 → 0.80 | 0.42 → 0.85 | 0.55 → 0.95 |
| `per_seg` | 每段贝塞尔的采样点 | 8 | 8 → 2 | 8 → **1** |
| `export.ring_count` | 每级环顶点数 | 16,12,8 → 8,6,4 | 同左 | 16,12,8 → 6,4,4,4 |

⚠️ fir 还把 `levels` 4 → 3：它 4100 根枝，管面本身就 60 万顶点，不降级怎么都进不了 6 万。
其余两棵结构完全没动（枝数 331 / 703 与高配一致）。

⚠️ **别为了省顶点去调小 `curveRes`**：采样变稀会让「父枝归属」的最近邻查询认错父亲
（实测 pine 直接报「枝 187 的父枝排在它后面」，成环），树的结构会坏掉。
降采样用 `per_seg`，它不改变贝塞尔骨架。

### 游戏档（`configs/game/`）—— 每棵约 5000 三角形

低配小游戏里"玩家会走近的英雄树"大概是 5k~15k 三角形。这一档把三棵树压到 **约 5.1k~5.3k**：

| 树 | 三角形 | 其中枝管 / 叶卡 | 顶点 | 枝 | 卡 |
|---|---|---|---|---|---|
| pine_game | 5,118 | 1,374 / 3,744 | 5,085 | 62 | 468 |
| oak_game | 5,274 | 3,018 / 2,256 | 4,452 | 135 | 282 |
| fir_game | 5,216 | 1,848 / 3,368 | 5,019 | 102 | 421 |

手段（在低配档基础上继续降）：`levels 2`（砍掉细枝层）、`per_seg 1`、`ring 3,3,3`、
`leaves` 调回 16~20、`leafScale` 放大到 2.2~2.6 —— **卡少十倍就只能靠"更少更大的叶簇"撑体积**，
这是低模树的标准做法（远看有体积、近看是纸片）。

⚠️ 这一档砍掉了一级分枝，松树/杉树会偏"长毛的柱子"。要保树形就保 `levels 3`，
代价是三角数涨到 ~7.5k（仍远低于低配档的 5.7 万）。

生成：

```bash
gen/shape_gen --config configs/game/pine.pbtxt --out output/veg_shapes/pine_game.vegshape
gen/shape_gen --config configs/game/oak.pbtxt  --out output/veg_shapes/oak_game.vegshape
gen/shape_gen --config configs/game/fir.pbtxt  --out output/veg_shapes/fir_game.vegshape
```

生成低配档（**采样密度与环数都在配置里**，命令行不用给任何参数）：

```bash
# pine: 50.6k 顶点
gen/shape_gen --config configs/low/pine.pbtxt --out output/veg_shapes/pine_low.vegshape
# oak:  53.6k
gen/shape_gen --config configs/low/oak.pbtxt  --out output/veg_shapes/oak_low.vegshape
# fir:  57.2k
gen/shape_gen --config configs/low/fir.pbtxt  --out output/veg_shapes/fir_low.vegshape
```

六份入库资产（`samples/*_{low,game}.vegshape`）就是用这些命令生成的（枝数/卡数/三角形
在 `samples/` 的说明里）。⚠️ **逐字节可复现的前提是同一个 Blender 版本**：几何来自 Blender
倒出的曲线，跨版本浮点会有 1e-6 量级的差（实测 Blender 4.0.2 重跑 ⇒ 字节不同、但转成文本
完全一致）。另外这六份资产历史上是「先出文本、再转二进制」的（`%.6g` 时代），
所以它们比 `shape_gen` 现在直接写出的二进制少几位有效数字 —— 数值等价，要严格逐字节
对齐就按上面的命令重出一遍（重出后 gold 需要一起刷新）。

钉住：`preset`、`scale`、`seed`、`levels`、`branches`、`leaves`、`leafScale`。
最常踩的两个坑：`baseSize` 是"第一根侧枝出现的高度**比例**"（不是尺寸，>1 会把枝挤到树顶）；
角度字段一律写**度**，长 4 的数组下标 = 枝级。

**② 出拓扑**

```bash
gen/shape_gen --config configs/mytree.pbtxt --out output/veg_shapes/mytree.vegshape
```
看摘要里的枝数/卡数是否合理；不满意就改 ① 的 `seed / branches / leaves` 再来一次（1 秒级）。

**③ 生图（提示词）**
叶子（**这两条合同必须写进提示词**）：
> 单张叶簇，居中构图，叶柄垂直朝下，**纯黑背景（背景像素接近 0）**，
> **叶子颜色明亮，不要有接近黑色的部分**，边缘干净，不要阴影和边框。

树皮：
> 无缝可平铺的树皮纹理，正面平视，均匀光照，不要透视和暗角。

**④ 归一化**

```bash
jpov_leaf_card raw_leaf.png out/leaf.png --anchor bottom_left --also-flip out/leaf_flip.png
make_generated_bark.sh raw_bark.png out/bark 1.5 0.15
```
把打印出来的 `leaf_aspect` 填回 ① 的 `export.leaf_aspect`，**再重跑一次 ②**（卡片宽高比在 shape 里）。

**⑤ 验收**

```bash
viewer --shape ... --bark out/bark --leaf out/leaf.png --leaf2 out/leaf_flip.png --leaf-black 32
```
判据：叶子**贴在枝上**（不悬空、不横躺）、树皮**没有横向接缝**、整体不像塑料。

**⑥ 收敛**
叶卡角度/缩放不对 → 回 ③ 调提示词（构图问题）或 ④ 换 `--anchor`（几何问题）；
枝形不对 → 回 ① 调参数（含 `noise` 块）。查看器**只负责显示**：面板里能调的是
呈现参数（法线强度、地面高度、层级、风），**叶面弯曲与枝条噪声都不在面板里** ——
它们是资产的一部分，改就改配置重新生成，否则预览和最终资产会变成两套真相。

### 几条硬规矩

- `--leaf-black` 必须与 `kLeafBlackThreshold`（32）一致；换值会让同一张素材的透明度变样。
- 归一化输出是**预乘 alpha**（RGB 已乘覆盖率）：消费方按黑底→alpha 读也等价，别再做一次反预乘。
- 镜像手性必须**单独出图**（`--also-flip`），引擎不做运行时镜像。
- 树皮一定要走过 seamless：u 每圈归零、v 沿枝累加，不平铺就会每圈一条硬缝。

## 命令行

| 参数 | 说明 |
|---|---|
| `--shape <vegshape>` | 拓扑（二进制资产；文本按扩展名也认）；省略 = `shapes/willow.vegshape`。**给了 `--glb` 时可省略/允许缺失**（纯 glb 查看模式） |
| `--glb <glb>` / `--glb-only` | 直接进 glb 展示态；`--glb-only` = 强制纯查看（不加载 shape，不能烘焙/回退） |
| `--bark <dir>` | 树皮材质目录；省略 = level 棕色 |
| `--leaf <png>` / `--leaf2 <png>` | 叶卡素材；`leaf2` 给**镜像手性**的 leaf type 1。少给 `leaf2` 会 WARNING 回退 |
| `--leaf-black <f>` | 黑底阈值（见上） |
| `--leaf-bend <a,b,c>` | 叶面弯曲覆盖：`along`(叶尖翘/垂) `,across`(叶面杯) `,crease`(中脉折痕)，单位 = 卡高；省略 = 用 shape 里 `leaf_types.bend` 的值 |
| `--leaf-normal <f>` | 由 alpha 生成的法线贴图强度 B（0 = 关） |
| `--normal-scale <f>` | 树皮法线强度（默认 1.5） |
| `--cam_phi/--cam_theta/--cam_r` | 视角（度、度、米；`cam_r=0` = 自动取景） |
| `--cam-target-y <f>` | 相机注视点高度（近看树冠/叶卡细节） |
| `--max-level <n>` / `--only-type <n>` | 只画到第 n 层枝 / 只画某个 leaf type |
| `--no-leaf` / `--no-bark` / `--no-hud` | 关叶图（看 debug 绿卡）/ 关树皮 / 关左上角相机 HUD |
| `--show-panel` | capture 时也画 UI 面板 |
| `--capture <dir>` | 无窗口出图：`veg_v2_front / _three_quarter / _side.png`（1280×720） |
| `--bake <out.glb>` | 烘焙一份 glb 后退出（headless；CI 的入口） |
| `--glb <in.glb>` | 启动即进 glb 展示态（带风调试面板） |
| `--no-wind` / `--wind-time <s>` | 关风 / 钉住风时间（A/B、可复现出图） |
| `--wind-amp <f>` / `--wind-turb <f>` | 覆盖 pivot 振幅 / 平动湍流幅度（A/B） |
| `--wind-blow <f>` / `--wind-blow-exp <f>` | 覆盖**吹歪**比例 / 指数（0 = 关） |

交互：左键拖 = 转视角 / 滚轮 = 推拉 / 中键上下拖 = 平移注视点（半屏 = 1m）；
`[` `]` 调最大层级、`P` 打印相机、`H` 打印叶卡帮助（各 type 的 `anchor_mode` / 推荐宽高比）。

## 目录

| 路径 | 内容 |
|---|---|
| `proto/shape.proto` | 资产格式（序列化边界）；`shape.h/.cc` 是运行时结构 + `Validate` + `LoadShapeFile` |
| `sweep.h/.cc` | 扫管（几何 + UV）与 `BuildLeafCards` / `ComputeCardFrame` / `CardVertexCount`（**anchor + anchor_mode → 姿态与 UV 的唯一实现**；叶卡弯曲也在这里按解析式求位置/法线/切线） |
| `viewer/` | `vegetation_v2_app.h` = 库接口（`RunViewer`）；`.cc` = 场景/渲染/交互；`vegetation_v2_viewer.cc` = 只有 CLI |
| `bake/` | **GLB 烘焙器**：shape(+bark+leaf) → 自洽 .glb（关节表 + 逐顶点 `(joint_id, phase)` + 内嵌贴图） |
| `leaf_texture.h/.cc` | 叶卡贴图 CPU 流水线（黑底→alpha / 法线 / PNG 编解码）——查看器与烘焙共用同一份像素 |
| `export_vegetation_shape.py` | **旧** Python 导出链（Blender 侧 python）：Sapling 曲线 → `ShapeProto` 文本；已由 `gen/` 的 C++ 实现取代，保留作对拍基准 |
| `dump_sapling_curve.py` | Blender 侧**只倒数据**：建树 + 曲线控制点 / 叶卡网格 → `.dump`（几何决策全在 `gen/`） |
| `gen/` | **导出链 C++ 实现**：`sapling_dump` 解析、`shape_builder` 建 shape、`shape_text` 写文本（仅人工查看）、`shape_gen` 入口、`shape_convert` 文本⇄二进制 |
| `leaf_card/` | **叶卡归一化**：生图 → 合规素材（根点识别 / 旋转 / 对齐裁剪）+ `leaf_card_spec.h`（生图合同） |
| `make_generated_bark.sh` | 生图树皮 → albedo(无缝)+normal+roughness（串起 `texture_tool` 的两个工具） |
| `proto/generate.proto` | **生成配置**的 schema（喂给 Sapling 的参数，逐字段写明含义/单位/效果） |
| `generate_shape.py` | **命令行入口**：`configs/*.pbtxt` → 调 Blender → 校验产物。纯逻辑可单测 |
| `generate_shape_test.py` | 上面那层的单测（不需要 Blender，秒级） |
| `configs/` | 入库的生成配置：`pine.pbtxt` / `oak.pbtxt` / `fir.pbtxt`（见其 README） |
| `make_bark_material.sh` | ambientCG JPG → 查看器认的 `albedo/normal/roughness.png` |
| `assets/` | 入库样例：`leaf/willow_leaf{,_flip}.png`、`bark/bark001/`（见其 README） |
| `samples/` | 范本拓扑（**全部 `.vegshape` 二进制**）：`willow`（183 枝/442 卡）、`willow_two_types`（215 枝/597 卡/2 type）；**入库档位**：`{pine,oak,fir}_low`（约 5 万顶点、带噪声，主力档）与 `{pine,oak,fir}_game`（约 5000 三角形）；**`willow_baked.glb`（12.5 MB，烘焙产物，见下）** |
| `docs/vegetation_asset_format.md` | **格式规范**：约定、扫管、UV、叶卡、装配、校验、验收、版本 |
| `bake/bake_options.h` | 烘焙参数/统计（baker 与关节构造共用，避免循环 include） |
| `bake/vegetation_glb_joints.{h,cc}` | **关节构造**（GL-free 纯几何）：抽稀 / soft / 父子链 / 相位场；烘焙与单测共用 |
| `bake/vegetation_glb_baker.{h,cc}` | 关节 → 树干/叶卡 primitive → 内嵌贴图 → 写 glb |

## 验收与回归

- **视觉**：跑上面的 `--capture`，用 `--cam-target-y 3 --cam_r 1.6` 看叶卡贴在枝上的位置。
- **gold test**：`bazel test //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_test`
  - `vegetation_v2_cards_1280x720.png`（debug 绿卡）：不透明盒子，8×8 ROI 阈值 **0.5**，精确锁叶卡摆放；
  - `vegetation_v2_leaf_1280x720.png`（贴叶+树皮）：alpha cutout 在 llvmpipe 下会**整块**跳到
    另一种离散状态（实测同一二进制两次渲染，4×4 ROI 最大差能到 124，约 0.08% 像素翻面），
    所以主判据改成**显著变化像素占比 < 2%**（真回归：去掉叶图 21.9%），ROI 只打印作诊断；
  - `vegetation_v2_glb_1280x720.png`（**glb 展示态**）：先 `--bake` 出 glb，再 `--glb` 载入、
    固定 `--wind-time 0.8` 出图。判据 ROI ≤ 2 **且** 显著像素 < 1% —— 认关节表算错、风没生效
    （实测关风 6.3%）、贴图没内嵌、顶点挂错枝；
  - 三张 gold **都由查看器二进制本身**跑出来（subprocess），保证和用户看到的是同一张图。
    渲染参数（`--normal-scale` / `--uv-vscale` / `--leaf-normal` / 风时间 / 相机）**全部显式钉住**，
    不许吃 app 默认值 —— 否则改默认值就会悄悄改写 gold，报警能力就没了。
    改观感后重生成：`bazel run //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_generator`。
- **烘焙/风**：`--bake` 后看日志（关节数 / 顶点数 / 叶卡到所属环的最大距离应 ≈ 1e-6 m）；
  再 `--glb <产物>` 用左下角面板调风，或 `--wind-time 0 / 1.2` 出两帧对比（同机位）。
  烘焙器不依赖 Blender：关节由 shape 的枝折线按弧长抽稀（粗枝 1.5m/细枝 0.5m、每枝 ≤8 个），
  逐顶点关节归属**按采样环精确给出**（树干按环下标 floor、叶卡按 anchor 的最近环），
  所以不存在 v1 那套 Voronoi 标签的跨链误判。
- **单测**：`bazel test //tools/jpov/tools/vegetation_tool_v2:all`
  （`shape_test` 校验合同含 anchor 离枝负例；`sweep_test` 含 4 种 `anchor_mode` 的 UV 锚点与
  `ComputeCardFrame` 右手系断言；`vegetation_glb_joints_test` 钉住关节表的三条不变式：
  `parent < id`、枝首枝尾必有枢轴、覆盖下标随采样单调 —— 每条都带负向对照）。

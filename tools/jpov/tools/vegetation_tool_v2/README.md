# vegetation_tool_v2 —— 植物**组合查看器**

把 `shape`（拓扑）+ `bark`（树皮材质）+ `leaf`（叶卡素材）组合成植物并渲染出来。
**人和 AI 都用它做资产验收**：三样给几样就渲染几样。

**两个状态**（右下「烘焙 glb」面板一键切换）：

| 状态 | 在画什么 | 用途 |
|---|---|---|
| **原材料态** | 现搭的 shape/bark/leaf（逐枝扫管 + 叶卡 quad） | 调拓扑/素材；叶卡可用 debug 绿卡看对齐 |
| **glb 展示态** | 刚烘焙出来的 `.glb`（树干 + 叶卡两个 primitive，带风链数据） | 看最终资产：树皮贴图、叶卡 alpha、**风**（左下「风调试」面板） |

烘焙产物是一份**自洽 .glb**：几何 + 内嵌贴图（树皮 albedo/normal/ORM、叶卡 albedo/normal）
+ 逐顶点 `(joint_id, phase)`（glTF `TEXCOORD_1`）+ 关节表（`node.extras.jpov_wind_joints`，
每关节 `pivot.xyz/parent/level/phase/soft`）。引擎 `LoadGltf` 读回即可直接吹风。

```
shape.pbtxt ──┐
bark/        ──┼─→ jpov_vegetation_v2_viewer ─→ 交互窗口 / 三张 capture PNG
leaf.png     ──┘
```

| 输入 | 必需 | 省略时 |
|---|---|---|
| `shape` = ShapeProto 文本（`.pbtxt`） | 有默认值 | 用打包目录里的 `shapes/willow.pbtxt` |
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
    --shape shapes/willow.pbtxt --leaf leaf/willow_leaf.png --bark bark/bark001
./jpov_vegetation_v2_viewer --glb output/veg.glb            # 交互：左下角风调试面板

# 纯 glb 查看模式：**没有 shape 也能开**（别人发来一个 glb，只想看看它长什么样）
cd anywhere && ./jpov_vegetation_v2_viewer --glb some.glb
# 在打包目录里（shapes/ 存在）想强制纯查看：加 --glb-only
./jpov_vegetation_v2_viewer --glb some.glb --glb-only
```

**纯 glb 查看模式**（2026-10-22 加）：`--glb-only`，或给了 `--glb` 且 **shape 文件找不到**
（默认的 `shapes/willow.pbtxt` 也不在 cwd）时只开 glb 展示态 —— 有地面、有风、有风调试面板，**没有**原材料态：面板上不会出现
「回到原材料态」，也不能烘焙（两者都需要 shape）。相机默认按 glb 包围盒自动取景。
渲染结果与「shape + glb」逐像素一致（同一机位实测 0 差异），只是少了回退能力。

**不用自己烘也能试**：仓库里带了一份烘焙产物
`samples/willow_baked.glb`（12 MB，692 关节 / 树干 40837 v / 叶卡 1768 v，内嵌树皮+叶卡贴图）。

```bash
cd tools/jpov && ./build_jpov_vegetation_v2_viewer.sh
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --glb samples/willow_baked.glb --glb-only
```

它就是用本目录的素材、按面板默认参数烘出来的：

```bash
./jpov_vegetation_v2_viewer --bake output/veg.glb \
    --shape shapes/willow.pbtxt --leaf leaf/willow_leaf.png --leaf-black 30 \
    --bark bark/bark001 --normal-scale 0.75
```

> 两份产物应当**逐字节相同**（md5 `e1a6a06f71fec3f49e8f2728bb4abb79`）。
> 若你的 md5 不同：先确认上面命令的参数一字不差（风/关节参数走面板默认值），
> 再确认 `AI_INFRA` 版本一致 —— 烘焙结果对几何与参数都很敏感。

- 打包目录里必须有 `fonts/` 才能画文字；**没有字体不会崩**，只是不画 HUD/标签。
- `--leaf-black`：黑底素材的「近乎黑 = 透明」阈值，软过档是 `[0.5t, 1.5t]`。
  入库样例背景是 `≈10~13` 的灰噪、内容 `≥44` ⇒ 给 **30**（`[15,45]`）；默认 12 会把灰噪留成半透明。

## 从配置生成 shape（Blender 侧）

上文那些 `.pbtxt` 是用 Blender + Sapling 离线长出来的。两条路，**产物等价**：

| 路径 | 入口 | 说明 | fir（4100 枝） |
|---|---|---|---|
| **C++（推荐）** | `gen/shape_gen` | Blender 只把曲线/叶卡网格倒成 `.dump`；采样、父枝、叶卡姿态、拓扑序、写 pbtxt 全在 C++ | **1.5 s** |
| Python（对照基准） | `generate_shape.py` | 原实现，留作对拍/回归基准 | 132 s |

```bash
# C++：配置 -> shape.pbtxt（Blender 只负责倒数据）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_gen \
    --config tools/jpov/tools/vegetation_tool_v2/configs/pine.pbtxt \
    --out output/veg_shapes/pine.pbtxt

# 只看配置有没有写对（不跑 Blender）
bazel run //tools/jpov/tools/vegetation_tool_v2/gen:shape_gen -- --help
```

| 层 | 位置 | 说明 |
|---|---|---|
| 配置（**输入**） | `configs/*.pbtxt`，schema 见 `proto/generate.proto` | 每个字段都写了含义/单位/效果预期；没写的字段 = 用预设自带值（proto2 optional 的存在性） |
| C++ 入口 | `gen/shape_gen.cc` | 读配置 → 调 Blender → 建 shape → 自检（唯一根 + 拓扑序）→ 写 pbtxt |
| C++ 几何 | `gen/shape_builder.cc` | 采样 / 父枝（`tools/util` 的稀疏栅格最近邻）/ 叶卡姿态 / anchor 合同 |
| Blender 侧 | `dump_sapling_curve.py` | **只做序列化**：建树 + 把曲线控制点和叶卡网格倒成 `.dump`，不做任何几何决策 |

- **Blender 不在仓库里**：它是镜像里的开发期工具（`/usr/bin/blender` 4.0.2），`--blender` 可改路径。
- 单测：`bazel test //tools/jpov/tools/vegetation_tool_v2/gen:all`
  （dump 解析的容错边界 + 合成 dump 的父枝/层数/环数/叶卡姿态/拓扑序/anchor）。
- 与 Python 产物的差异：几何逐行一致（实测 93424 行里 0 处结构不同，最大数值差 1e-6 量级），
  唯一例外是「卡片正好骑在枝上」时根/梢方向在数学上无定义，约 1% 的卡在两种浮点精度下取反。
- 生成配置的取舍与已知缺陷见 [`configs/README.md`](configs/README.md)。

## 生图资产 → 合规素材（叶卡 / 树皮）

生图模型不知道我们的格子/UV 约定，所以**出图之后必须过一道归一化**。两个工具：

```bash
# 叶卡：任意构图的黑底叶子 → 合规素材（根点落在 anchor 角上 + 内容撑满宽度）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/leaf_card/jpov_leaf_card \
    /path/leaf_raw.png output/leaf/willow_leaf_norm.png --anchor bottom_left

# 树皮：一张树皮图 → albedo(无缝) + normal + roughness 三件套
./tools/jpov/tools/vegetation_tool_v2/make_generated_bark.sh \
    /path/bark_raw.png output/bark/gen001 1.5 0.15
```

**叶卡的生图合同**（数字唯一定义在 [`leaf_card/leaf_card_spec.h`](leaf_card/leaf_card_spec.h)）：

| 项 | 要求 | 判据 |
|---|---|---|
| 背景 | 纯黑 | `max(r,g,b) <= 16` |
| 内容 | 叶子颜色**避开黑色** | `max(r,g,b) >= 48` |

提示词里直接抄这两条（叶子画暗了会被判成透明、破洞）。归一化会体检并报出「灰区占比」。

工具做的事（`leaf_card/leaf_card_normalize.h` 有完整说明）：
1. 按合同切内容 → 2. 找叶柄（主轴两端里**更细**的那端）→
3. 旋转到「从 anchor 角指向格子内部」（角锚点 45°、边中点 90°）→
4. 裁剪使**根点正好落在锚点**上，并报出被裁掉的比例（>0 说明这张图不适合该 anchor）。

输出诊断里的 `leaf_aspect` 就是该写进 shape 配置的值（`export.leaf_aspect`，宽/高）。

**树皮的坑**：UV 的 u/v 两向都靠 `GL_REPEAT` 平铺，图不无缝每圈一条硬缝；
`roughness` 生图给不出来，脚本按「暗 = 更粗糙」从 albedo 推（近似，但比全 0.5 强）。

## 命令行

| 参数 | 说明 |
|---|---|
| `--shape <pbtxt>` | 拓扑；省略 = `shapes/willow.pbtxt`。**给了 `--glb` 时可省略/允许缺失**（纯 glb 查看模式） |
| `--glb <glb>` / `--glb-only` | 直接进 glb 展示态；`--glb-only` = 强制纯查看（不加载 shape，不能烘焙/回退） |
| `--bark <dir>` | 树皮材质目录；省略 = level 棕色 |
| `--leaf <png>` / `--leaf2 <png>` | 叶卡素材；`leaf2` 给**镜像手性**的 leaf type 1。少给 `leaf2` 会 WARNING 回退 |
| `--leaf-black <f>` | 黑底阈值（见上） |
| `--curl <f>` | 顶点法线球面化强度 A（0 = 平面法线） |
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
| `sweep.h/.cc` | 扫管（几何 + UV）与 `BuildLeafCards` / `ComputeCardFrame`（**anchor + anchor_mode → 姿态与 UV 的唯一实现**） |
| `viewer/` | `vegetation_v2_app.h` = 库接口（`RunViewer`）；`.cc` = 场景/渲染/交互；`vegetation_v2_viewer.cc` = 只有 CLI |
| `bake/` | **GLB 烘焙器**：shape(+bark+leaf) → 自洽 .glb（关节表 + 逐顶点 `(joint_id, phase)` + 内嵌贴图） |
| `leaf_texture.h/.cc` | 叶卡贴图 CPU 流水线（黑底→alpha / 法线 / PNG 编解码）——查看器与烘焙共用同一份像素 |
| `export_vegetation_shape.py` | **旧** Python 导出链（Blender 侧 python）：Sapling 曲线 → `ShapeProto` 文本；已由 `gen/` 的 C++ 实现取代，保留作对拍基准 |
| `dump_sapling_curve.py` | Blender 侧**只倒数据**：建树 + 曲线控制点 / 叶卡网格 → `.dump`（几何决策全在 `gen/`） |
| `gen/` | **导出链 C++ 实现**：`sapling_dump` 解析、`shape_builder` 建 shape、`shape_text` 写 pbtxt、`shape_gen` 入口 |
| `leaf_card/` | **叶卡归一化**：生图 → 合规素材（根点识别 / 旋转 / 对齐裁剪）+ `leaf_card_spec.h`（生图合同） |
| `make_generated_bark.sh` | 生图树皮 → albedo(无缝)+normal+roughness（串起 `texture_tool` 的两个工具） |
| `proto/generate.proto` | **生成配置**的 schema（喂给 Sapling 的参数，逐字段写明含义/单位/效果） |
| `generate_shape.py` | **命令行入口**：`configs/*.pbtxt` → 调 Blender → 校验产物。纯逻辑可单测 |
| `generate_shape_test.py` | 上面那层的单测（不需要 Blender，秒级） |
| `configs/` | 入库的生成配置：`pine.pbtxt` / `oak.pbtxt` / `fir.pbtxt`（见其 README） |
| `make_bark_material.sh` | ambientCG JPG → 查看器认的 `albedo/normal/roughness.png` |
| `assets/` | 入库样例：`leaf/willow_leaf{,_flip}.png`、`bark/bark001/`（见其 README） |
| `samples/` | 范本拓扑：`willow.pbtxt`（183 枝/442 卡/1 type）、`willow_two_types.pbtxt`（215 枝/597 卡/2 type，另一棵树）；**`willow_baked.glb`（12 MB，入库的烘焙产物，见下）** |
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

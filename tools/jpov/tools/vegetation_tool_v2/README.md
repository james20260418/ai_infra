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

- 打包目录里必须有 `fonts/` 才能画文字；**没有字体不会崩**，只是不画 HUD/标签。
- `--leaf-black`：黑底素材的「近乎黑 = 透明」阈值，软过档是 `[0.5t, 1.5t]`。
  入库样例背景是 `≈10~13` 的灰噪、内容 `≥44` ⇒ 给 **30**（`[15,45]`）；默认 12 会把灰噪留成半透明。

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
| `export_vegetation_shape.py` | Blender/Sapling → `ShapeProto` 文本（含 `--anchor-mode`、导出侧的 anchor 上枝校验） |
| `make_bark_material.sh` | ambientCG JPG → 查看器认的 `albedo/normal/roughness.png` |
| `assets/` | 入库样例：`leaf/willow_leaf{,_flip}.png`、`bark/bark001/`（见其 README） |
| `samples/` | 范本拓扑：`willow.pbtxt`（183 枝/442 卡/1 type）、`willow_two_types.pbtxt`（215 枝/597 卡/2 type，另一棵树） |
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

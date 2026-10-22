# vegetation_tool_v2 —— 植物**组合查看器**

把 `shape`（拓扑）+ `bark`（树皮材质）+ `leaf`（叶卡素材）组合成植物并渲染出来。
**人和 AI 都用它做资产验收**：三样给几样就渲染几样。

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
```

- 打包目录里必须有 `fonts/` 才能画文字；**没有字体不会崩**，只是不画 HUD/标签。
- `--leaf-black`：黑底素材的「近乎黑 = 透明」阈值，软过档是 `[0.5t, 1.5t]`。
  入库样例背景是 `≈10~13` 的灰噪、内容 `≥44` ⇒ 给 **30**（`[15,45]`）；默认 12 会把灰噪留成半透明。

## 命令行

| 参数 | 说明 |
|---|---|
| `--shape <pbtxt>` | 拓扑；省略 = `shapes/willow.pbtxt` |
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

交互：左键拖 = 转视角 / 滚轮 = 推拉 / 中键上下拖 = 平移注视点（半屏 = 1m）；
`[` `]` 调最大层级、`P` 打印相机、`H` 打印叶卡帮助（各 type 的 `anchor_mode` / 推荐宽高比）。

## 目录

| 路径 | 内容 |
|---|---|
| `proto/shape.proto` | 资产格式（序列化边界）；`shape.h/.cc` 是运行时结构 + `Validate` + `LoadShapeFile` |
| `sweep.h/.cc` | 扫管（几何 + UV）与 `BuildLeafCards` / `ComputeCardFrame`（**anchor + anchor_mode → 姿态与 UV 的唯一实现**） |
| `viewer/` | `vegetation_v2_app.h` = 库接口（`RunViewer`）；`.cc` = 场景/渲染/交互；`vegetation_v2_viewer.cc` = 只有 CLI |
| `export_vegetation_shape.py` | Blender/Sapling → `ShapeProto` 文本（含 `--anchor-mode`、导出侧的 anchor 上枝校验） |
| `make_bark_material.sh` | ambientCG JPG → 查看器认的 `albedo/normal/roughness.png` |
| `assets/` | 入库样例：`leaf/willow_leaf{,_flip}.png`、`bark/bark001/`（见其 README） |
| `samples/` | 范本拓扑：`willow.pbtxt`（183 枝/442 卡/1 type）、`willow_two_types.pbtxt`（215 枝/597 卡/2 type，另一棵树） |
| `docs/vegetation_asset_format.md` | **格式规范**：约定、扫管、UV、叶卡、装配、校验、验收、版本 |

## 验收与回归

- **视觉**：跑上面的 `--capture`，用 `--cam-target-y 3 --cam_r 1.6` 看叶卡贴在枝上的位置。
- **gold test**：`bazel test //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_test`
  - `vegetation_v2_cards_1280x720.png`（debug 绿卡）：不透明盒子，8×8 ROI 阈值 **0.5**，精确锁叶卡摆放；
  - `vegetation_v2_leaf_1280x720.png`（贴叶+树皮）：alpha cutout 在 llvmpipe 下会跳状态，
    用 **4×4** ROI（更粗的块摊平抖动）+ 阈值 **15** 当烟雾报警（没贴树皮 25.8 / 叶子被杀 20.5 / 没贴叶图 41.5）。
  - 两张 gold **都由查看器二进制本身**跑出来（subprocess），保证和用户看到的是同一张图。
    改观感后重生成：`bazel run //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_generator`。
- **单测**：`bazel test //tools/jpov/tools/vegetation_tool_v2:all`
  （`shape_test` 校验合同含 anchor 离枝负例；`sweep_test` 含 4 种 `anchor_mode` 的 UV 锚点与 `ComputeCardFrame` 右手系断言）。

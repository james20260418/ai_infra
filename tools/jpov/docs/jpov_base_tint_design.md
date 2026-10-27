# 顶点 base tint（分区换色）设计 — MVP

> 2026-10-27 实现（PR 待合）。用户需求（Danis）：
> 「mesh 属性这件事，需要 clothing tool 里增设顶点配置覆盖的编辑按钮：用户选择一个 byte，
> 然后画笔涂一些点，clothing tool 负责把顶点的属性做成接收该 id 的 tint，染白这个不放 tool，
> 我判断建模时搞白更自然些。MVP 只实现这个。model viewer 和 clothing tool 里都加 2 个调色板
> RGB 换色 + 一个 enable toggle。」

## 1. 数据布局

| 层 | 内容 |
|---|---|
| CPU `MeshData` | `MeshVertexFlags::kBaseTint` + `std::vector<uint8_t> base_tint`（逐顶点 0..`kNumBaseTintColors`） |
| GPU 顶点 `loc6` | **统一 vec4**：`(lag_ratio 或 joint_id, max_lag 或 phase, tint 索引, 预留)` |
| 顶点着色器 | `flat out int vTintIndex = int(aFlex.z + 0.5)`（flat = provoking vertex） |
| 片元着色器 | `uTintEnabled==1` 时 `base_color *= (id==1 ? uTintColor0 : uTintColor1)` |
| 材质 `PBRMaterial` | `BaseTint base_tint { bool enabled; Color color[2]; }`（**运行期**，不进 glb） |
| glb 自定义属性 | `_JPOV_TINT`：`UNSIGNED_BYTE SCALAR`，每顶点一个 id |

**id 语义**：`0` = 不换色（保持 baseColor 原色）；`1..N` = 用第 i 个调色板色乘 baseColor。
MVP `N = 2`。byte 的值空间是 0..255，扩通道数只需扩 `kNumBaseTintColors` 与 shader 的
uniform 数，**不需要改顶点布局**。

**为什么共用 loc6**：风（`kWind`）只用 `.xy`，布料松弛度（`kRelax`）只用 `.xy`，所以 `.z` 空出来。
`kBaseTint` 与两者都可共存（一棵树既能吹风又能按季换色）。`kRelax` 与 `kWind` 仍互斥（同抢 `.xy`）。

## 2. 着色语义与开关

- 只乘 **baseColor**（贴图或常值），不碰 metallic / roughness / normal / AO / emissive。
- `enabled == false` ⇒ 片元着色器**整段跳过**（不是乘白），供 A/B 对照；引擎默认 false（零成本），
  两个工具默认 true（颜色全白 = 恒等，改色才可见）。
- 逐顶点 id 用 **flat** 读 ⇒ 一个三角形整体一个色；画笔在三角形边界上左右不一致时，边界由
  provoking vertex 决定（MVP 接受；工具侧只做逐顶点赋值）。

## 3. 工具侧

**clothing tool**（`tools/jpov/clothing/`）
- 面板「顶点 Tint 覆盖（刷选区）」：**复用 3D 画笔的选区**，三个按钮 **刷 Tint 1 / 刷 Tint 2 / 清除 (0)**
  一键把选区内顶点刷成对应 id（无独立涂抹模式、无第二个画笔半径）。
- 面板「Tint 调色 / 显示」：两个 RGB 调色板 + enable + 「显示 tint 分区」；与刷色面板**分开**
  （2026-10-27 Danis：调色/显示 和 刷颜色 不要挤在一个面板）。
- 点刷色按钮时若 mesh 没有 `kBaseTint`，**一次性**补全 0 并重新注册 GPU mesh（之后只 `UpdateMesh`）。
- 写 `cloth_current_[i].mesh.base_tint` 并立即推 GPU；保存 glb 时写 `_JPOV_TINT`。
- **不做染白**：baseColor 贴图由建模侧留白/涂白（白 × 用户色 = 用户色）。

**model viewer**（`tools/jpov/demo/viewer_app.h`）
- 底部面板改成两列：左列原光照/场景滑条；右列两个 RGB 调色板 + enable。
- 每帧把调色板写进各 primitive 的 `PBRMaterial.base_tint`；逐顶点 id 来自 glb。

## 4. 边界（MVP 明确不做）

- **只有 object3d 着色器消费 tint**。instanced 与 skinned 路径未接入：
  - clothing tool 里开启「随机摆动测试」且衣服带蒙皮时会走蒙皮路径 ⇒ tint 暂时不显示；
  - 关掉摆动测试（静态绘制）即回到 object3d 路径，tint 正常。
- 调色板颜色**不落盘**（glb 只存 id）；换工具/换会话需要重新调色。
- 没有逐顶点颜色混合（flat 硬边界），也没有 tint 的 roughness/metallic 联动。
- 补洞产生的新顶点取端点 A 的 id（分类量不能取平均）。

## 5. 验证

- `jpov_object3d_tint_test`（GPU 像素校验）：id0 白 / id1 红 / id2 绿 / id1+disabled 与 id0 逐通道相同。
- `jpov_gltf_saver_test.RoundTripBaseTintAttribute`：`_JPOV_TINT` 写→读回逐顶点相等。
- `brush_tool_test`：一键刷选区只改选中顶点、同值重复刷返回 0（驱动 GPU 重传的判据）、id=0 清除。

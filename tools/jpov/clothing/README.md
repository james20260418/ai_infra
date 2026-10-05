# JPOV 穿衣工具（clothing tool）使用说明

> 面向 **人类与 AI**。把一件（通常由 gen3d 生成的）衣服模型**对齐到人体**、**自动蒙皮**、
> 再**保存成带骨骼的 glb**。
> 代码：`tools/jpov/clothing/`（独立包 `jpov::clothing`）；编译脚本：`tools/jpov/clothing_tool.sh`
> （本说明即从该脚本引用）。

---

## 1. 一句话流程
**打开（人体 + 衣服）→ 对齐贴合 → 一键蒙皮 → 保存 glb。**

## 2. 编译
```bash
cd <repo 根>
bash tools/jpov/clothing_tool.sh        # 编译到 output/jpov_clothing_tool/
```

## 3. 运行（示例：Mixamo 男性 + 战术背心，照抄即可试）
```bash
output/jpov_clothing_tool/jpov_clothing_tool \
  --body_reference_path tools/jpov/test/object3d/mixamo_male/mixamo_male.glb \
  --cloth_path          tools/jpov/test/object3d/clothing/harness_vest/harness_vest.glb
```
| 参数 | 说明 |
|---|---|
| `--body_reference_path <glb>` | 人体参考（**带骨架**）。缺省 = 项目自带 Mixamo 男性。 |
| `--cloth_path <glb>` | 衣服模型（**必填**）。 |
| `--phi_deg <度>` | 初始俯视角（默认 20）。 |

交互：**右键 drag 旋转视角、滚轮 zoom**。

## 4. 界面
- **左上 — 变换 / 保存 / 场景**
  - 平移 X/Y/Z：步长输入框 + `<` `>` 按钮
  - 旋转 RX/RY/RZ：步长输入框（度）+ `<` `>` 按钮（绕轴逆时针）
  - 整体缩放：系数框 + `-` `+`
  - 按钮 **保存衣服 glb**
  - 地面高度滑条；勾选「显示人体 / 显示衣服」
  - *变换即时生效，仿真进行中也能调。*
- **右上 — 软体仿真**：重力 g / 总质量 M / 力系数 F / 衰减 k / 速度上限；人体排斥（开关 + buffer + 切向速度保留系数）；继续/暂停、重置衣服。
- **右下 — 软布自动蒙皮**：按钮 **一键蒙皮**；勾选 **权重生长（种子扩散）**（默认开）；**种子半径(mm)**（默认 10）；**焊接容差(mm)**（默认 5）。

## 5. 推荐操作顺序
1. 运行（§3）。
2. 用左上变换把**衣服对齐、贴合到人体**（后续蒙皮质量的前提）。
3. （可选）用右上仿真让衣服自然贴合 / 下垂。
4. 右下 **一键蒙皮**。*蒙皮后几何冻结*（禁变换 / 仿真 / 重置）—— 想改就重开。
5. **保存衣服 glb** → 结果写在**源衣服同目录**，文件名 `<原名>_cloth_edit<时间戳>.glb`。

## 6. 蒙皮参数（右下）
| 参数 | 默认 | 含义 |
|---|---|---|
| 权重生长（种子扩散） | 开 | 种子冻结的调和扩散：离体顶点从种子「生长」出权重；关掉 = 只用逐顶点直接投影权重 |
| 种子半径 seed_eps (mm) | 10 | 到身体距离 ≤ 该值视为「贴身」→ **种子**（权重冻结）；更远的离体顶点由生长补出 |
| 焊接容差 (mm) | 5 | 相距 ≤ 该值的顶点视为同一「缝合点」（导出器在 UV/材质缝合处拆开的重复顶点）→ 焊接成组，消除接缝**开裂** |

- **衣服表面开裂** → 调大「焊接容差」（5mm 实测更优）。
- **离体/悬空处权重乱**（种子太少、生长范围过大）→ 视情况调「种子半径」。

## 7. 成品样例（成功典范，已入库）
`tools/jpov/test/object3d/clothing/harness_vest/harness_vest_skinned_sample.glb`
—— harness_vest 对齐到 mixamo_male、一键蒙皮后保存的**标准成品**（带 skin / 24 骨）。
作为「正确结果」参考与回归对照。查看：
```bash
output/jpov_clothing_tool/jpov_clothing_tool \
  --body_reference_path tools/jpov/test/object3d/mixamo_male/mixamo_male.glb \
  --cloth_path tools/jpov/test/object3d/clothing/harness_vest/harness_vest_skinned_sample.glb
```
（也可用任意 glTF 查看器 / Blender / fbx_viewer 打开；带 skin，可直接接渲染管线。）

## 8. 边界
- 面向**贴身软布**；宽松 / 叠穿非本工具目标（离体顶点靠生长 + 兜底）。
- 蒙皮后**几何冻结**，权重↔顶点对应不可再变。
- 结果 glb 含 skin + inverseBindMatrices，直接在 fbx_viewer / 渲染管线使用。

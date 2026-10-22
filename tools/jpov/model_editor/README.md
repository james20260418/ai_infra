# JPOV 模型编辑器（model editor · 裁剪）

从穿衣工具（`tools/jpov/clothing/`）**复制做减法**而来的独立小工具：加载 reference
（可空）+ target 两个模型，对 target 做**平移 / 旋转 / 缩放**与**沿坐标平面裁剪**，结果
可保存成 glb。命名空间 `jpov::model_editor`、目录 `tools/jpov/model_editor/` 均独立一整套。

> 定位：给「瑜伽裤裤腿下方开口」这类**裁一刀**的需求用。本工具已完全取代早先的
> 旧 model editor（原 `tools/jpov/demo/editor/`，已随其 build 脚本一并删除）。

## 编译 / 运行

```bash
./tools/jpov/model_editor.sh
# → output/jpov_model_editor/jpov_model_editor

output/jpov_model_editor/jpov_model_editor \
    --target_path /path/to/target.glb \
    [--reference_path /path/to/reference.glb]
```

- `--target_path`：**必填**，被编辑的模型。
- `--reference_path`：可选；缺省无 reference（只显示 target）。
- `--phi_deg`：初始俯视角（度，默认 20）。

headless UI 自检（不弹窗，出单张带面板的图；可脚本化复现裁剪）：

```bash
output/jpov_model_editor/jpov_model_editor --ui_shot --output_dir /tmp/ui \
    --target_path /path/to/target.glb \
    [--clip_axis x|y|z --clip_coord V --clip_side delete_low|delete_high] [--save_after_clip]
# → /tmp/ui/model_editor_ui.png
```

- `--clip_axis`：裁剪坐标轴（`x`/`y`/`z`，默认 `y`）。
- `--clip_coord`：裁剪面在该轴上的坐标；不给则用该轴包围盒范围中点。
- `--clip_side`：删除哪侧（`delete_low` 删坐标小的一侧 / `delete_high` 删大的一侧）。

## 交互

- **右键 drag**：相机环绕；**滚轮**：缩放。
- **左上角面板**：target 平移 / 旋转 / 缩放（**步进式**：步长输入框 + `<` `>` 按钮）；
  `保存 target glb`；地面高度；显示开关（reference / target / 坐标轴 / 缝合面）；`重置 target`
  （撤销全部变换 / 裁剪 / 缝合，回到加载态）。
  「显示坐标轴」在世界原点画红/黄/蓝三根 1m 单位矢量（横截面 2cm）与 X/Y/Z 3D 标签（文字高度 0.2m），
  用于核对朝向（实现见 `interface/axis_frame.h`）。
- **右侧面板**：裁剪坐标轴选择（`X` / `Y` / `Z`）；裁剪面坐标**步进**（步长输入框 + `<` `>`
  按钮，不再用滑条 —— 滑条范围只有启动包围盒，缩放/移动后够不着）；`删除 <轴> < 侧` 与
  `删除 <轴> > 侧` 两个按钮；`显示裁剪面` 开关；结果 / 错误提示；当前三角形数。
- **缝合面板**：算法下拉、缝合面 y（同样的步进方式）、影响带、形变区 `y_span`、对应旋转偏移、
  `执行缝合`、`reference 也切到缝合面`、`显示缝合面` 开关、指标回显。
- 两块半透明面（裁剪面 / 缝合面）的尺寸**每帧按 target 当前 AABB 现算**（×1.05 余量），
  所以 target 平移 / 缩放 / 裁剪 / 缝合之后会跟着变。
- **中键 drag**：纵向 pan（半屏 = 1m，相机 position 与 target 同步平移）。
- **诊断 HUD**（视口上方中间，截图可复现配置）：`CAMERA`（pos / R / phi / theta / panY / target /
  fov / near / far / aspect）、`STATE`（clip / seamY / algo / band / off / refCut）、
  `MOVE`（用户累计平移 + 缩放系数）、`M`（累积模型矩阵 4 行）、`MVP`（4 行）。

## 缝合语义（`mesh_seam.h`，2026-10-22）

用户把 target 与 reference 摆好相对位置，给**一个**水平面 `y`：target 保留上方、reference
保留下方，各自露出断面；然后把 target 断面附近的顶点做形变雕刻，使它的底口**形状 + 位置**
严格等于 reference 的顶口，并尽量让法线连续（例：路灯上半截焊到蓝人脖子上）。

- 三种形变算法（面板 `算法` / CLI `--stitch_algo`）：
  - `harmonic`：带内解 Laplace（位移调和扩散），最稳；
  - `arap`：As-Rigid-As-Possible，形变大时不易塌陷/翻面；
  - `tps`：断面平面内 2D 薄板样条（RBF）全局场。
- 断面环：按位置焊接后只取「落在切平面上」的边界边串环；非单环 / 自交 / 退化**直接拒绝**。
- 环重建（`rebuild_loop`）：把 target 断面按 reference 环的顶点序列细分（分裂边界边，
  局部扇形三角化）⇒ 两边顶点一一对应，可直接按索引焊接。要求 target 环顶点数 ≤ reference。
- 指标：断面 Hausdorff / 法线夹角均值与最大 / 翻面数（面板与日志都会回显）。

headless：

```bash
output/jpov_model_editor/jpov_model_editor --ui_shot --output_dir ../out \
    --target_path street_lamp.glb --reference_path mixamo_male.glb \
    --target_scale 0.1 --target_move -3.441,-11.3344,0 \
    --stitch_y 0.88 --stitch_algo arap
```

## 诊断 HUD（截图复现配置）

视口上方中间常驻一块半透明 HUD（`DrawHud`），内容：

- `CAMERA pos(...) R=... phi=... theta=...` + `target(...) fov/aspect` —— 相机姿态；
- `STATE clip=Y0.130 seamY=0.870 algo=arap band=2.00 off=0.0 refCut=1` —— 当前裁剪/缝合配置；
- `MOVE t(...) scale=...` —— 用户累计平移 / 缩放系数；
- `M`（4 行）—— 累积模型矩阵（行主序，p' = M·p；平移/旋转/缩放按烘焙顺序左乘累积）；
- `MVP`（4 行）—— P·V·M（fov 60、near 0.05、far 1000、aspect = 窗口宽高比）。

用途：出问题时截图即可复现"是哪一组变换 / 哪个相机"。

## 裁剪语义（`mesh_clip.h`）

沿所选坐标轴（`X` / `Y` / `Z`）的平面把网格切成两半，**保留一侧、删除另一侧**；
透明裁剪面的尺寸 = target 的**包围盒大小**（面内两轴铺满包围盒），便于判断切口位置。
裁剪坐标轴与判据在 `ClipMeshByAxis`：`axis ∈ {0,1,2}`；`ClipKeepSide{kGreater,kLess}`
分别表示保留坐标大 / 小的一侧。

- 三角形整体在保留侧 → 原样保留；整体在删除侧 → 丢弃；**跨面** → 截断。
- 跨面三角形用 Sutherland–Hodgman 半空间裁剪裁成 3 / 4 边形，再扇形三角化；
  新边界顶点由交叉边的两个端点**按参数 t 插值**所有逐顶点属性：
  - 位置（线性）、法线（插值后重新归一化）、UV（线性）、切线（线性）；
  - 骨权：把两端点的 `(joint, weight)` 合并，按 `(1-t)/t` 加权，取权重最大的 4 个，
    再按保留的 4 个之和归一。
- 输出**索引化**：原始顶点按原下标去重；边界顶点按「无序边」去重（相邻三角形共享边只
  建一次），保证缝合处不出裂缝。
- **裁空**（结果三角形数为 0）→ 上层**报错并不做**（保持几何不变）。

## 保存（`model_save.{h,cc}`）

把当前 target 几何（含变换 / 裁剪结果）写成 `<stem>_model_edit<时间戳>.glb`，落在源
glb 同目录；target 自带骨架时一并写入 skin。后台线程写文件，不阻塞交互。

## 文件

| 文件 | 职责 |
|---|---|
| `mesh_clip.h` | 沿 X/Y/Z 坐标平面裁剪三角形（纯函数 / GL-free） |
| `mesh_seam.h` | 断面缝合：环提取 / 对应 / 三种形变 / 环重建（纯函数 / GL-free） |
| `model_transform.h` | target 平移 / 旋转 / 缩放（就地改顶点，纯函数） |
| `number_input.h` | 数值输入解析（纯函数） |
| `model_save.{h,cc}` | 保存 glb（后台线程 + 状态机） |
| `model_editor_app.h` | 渲染核心 App（场景 + 面板 + 唯一 `OneIteration`） |
| `jpov_model_editor.cc` | 主程序（CLI 装配 + 交互 / headless 分发） |

## 与穿衣工具的差异（减法）

去掉了软体仿真 / 自动蒙皮（weight transfer）/ 人体排斥 / 最近邻三角形建图——本工具都
用不上。保留：`reference`+`target` 加载、target 变换、保存。新增：裁剪。

## 测试

```bash
bazel test //tools/jpov/model_editor:model_transform_test   # 变换纯函数
bazel test //tools/jpov/model_editor:mesh_clip_test         # 裁剪纯函数 + yoga_pants 端到端
```

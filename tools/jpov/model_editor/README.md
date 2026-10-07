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
  `保存 target glb`；地面高度；显示开关；`重置 target`（撤销全部变换 / 裁剪，回到加载态）。
- **右侧面板**：裁剪坐标轴选择（`X` / `Y` / `Z`）；裁剪面坐标滑条（范围 = 该轴包围盒）；
  `删除 <轴> < 侧` 与 `删除 <轴> > 侧` 两个按钮；`显示裁剪面` 开关（裁剪面半透明块可能影响
  观感，可直接关掉显示）；结果 / 错误提示；当前三角形数。

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

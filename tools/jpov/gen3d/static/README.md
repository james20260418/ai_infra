# gen3d/static 使用说明（从这句开始读）

> 这是 gen3d 顶层的 **static（静态 PBR 模型生熟）** 子目录；带骨骼生熟（mixamorig rig）见
> sibling 目录 `gen3d/skeleton/`。本 README 面向第一次接触**静态**模型生熟、想"生成一个静态
> 3D 模型并看到它的图"的人。
> 按下面步骤照做即可出图，**不需要先读源码**。想看内部原理再翻文末"进阶文档"。

## 本文件夹是什么

`gen3d/static` 是 JPOV 的工具：给一段文字（prompt）**或一张/多张参考图**，
自动去 Tripo 生成一个**静态 PBR 的 3D 模型文件（.glb）**，再用 JPOV 渲染成
4 张预览图给你看。它**必须配合本仓库的代码和 bazel 才能用**，不能脱离仓库独立运行。

```
文字描述        ──┐
单张参考图      ──┼──> gen3d/static ──> 模型文件 .glb + 4 张预览图 .png
多视图参考图    ──┘
```

三种输入方式**互斥**（选一种）：文本 `--prompt` / 单图 `--image` / 多视图 `--images`。

## 前置条件（先确认，缺一不可）

1. 在 JPOV 仓库根目录（`/james_pm/ai_infra_2`）下操作。
2. 环境变量 `TRIPO_API_KEY` 已设置（调 Tripo 需 key，会烧 credits）。
   检查：`echo $TRIPO_API_KEY` 非空即可。没设先去设。
3. 有 bazel；渲染需要显示（无 DISPLAY 会自动起 Xvfb，见 gen3d_static.sh）。

## 方法 A：一键全链（推荐，只跑这一条命令）

在仓库根目录执行：

```bash
bash tools/jpov/gen3d_static.sh <output_dir> <名字> --prompt "你想生成的模型的描述"
```

例（文本）：

```bash
bash tools/jpov/gen3d_static.sh output/gen3d indoor_column \
    --prompt "浅白色大理石柱，圆柱主体，上下有方形基座"
```

例（单张参考图）：

```bash
bash tools/jpov/gen3d_static.sh output/gen3d indoor_column \
    --image /path/to/ref.png
```

例（多视图参考图，顺序固定 front / left / back / right）：

```bash
bash tools/jpov/gen3d_static.sh output/gen3d indoor_column \
    --images /path/to/front.png /path/to/left.png /path/to/back.png /path/to/right.png
```

做了什么：

1. 自动 bazel 编译 gen3d_cmd + model_viewer
2. **（图像模式）先做本地图片审查**——不合格直接中止，不发 HTTP、不烧 credit
3. 调 Tripo 生成模型 → 下载到 `output/gen3d/indoor_column.glb`
4. 用 JPOV 渲染 4 视角 → `indoor_column_{front,up,left,perspective}.png`
5. 最后打印**全部产物绝对路径**，照清单去拿。

**常用可选参数**：
| 参数 | 作用 |
|---|---|
| `--triangles <n>` | 面数预算，默认约 4000；范围 50~20000 |
| `--real_size` | 按真实物理尺寸（米）输出（如"柱子高5米"会生效） |
| `--negative "..."` | 排除不想出现的内容（**仅文本模式**） |
| `--high_poly` | 关掉低模默认（本次仍映射同档，H 档为后续扩展） |
| `--align_to_image` | （图像模式）把模型对齐到参考图的观察视角 |
| `--skip_image_check` | **跳过本地图片审查（危险：可能白烧 credit，仅供调试）** |

跑完想验证模型真的好 → 直接看 4 张 PNG；或 `file output/gen3d/<名字>.glb`
应显示 `glTF binary model, version 2`。

## 参考图要求（**找图/画图前必读**）

图像模式下，图片路径**由调用方自己声明**（本工具不自动扫目录、不猜文件名）。
提交前工具会先在**本地**审查，不合格**直接拒绝、不发 HTTP、不消耗 credit**。

### 硬性规则（不满足会被本地审查拦下）

| 项 | 规则 |
|---|---|
| 格式 | **PNG / JPEG / WebP**（按文件真实内容判定，不只看扩展名） |
| 文件大小 | **≤ 20 MB** |
| 分辨率 | **每边 ≥ 256 px**；建议 ≥ 512 px |
| 分辨率上限 | 每边 ≤ 6000 px |
| 文件 | 必须存在、非空、是常规文件 |

审查不通过时，工具会逐张打印失败原因，并明确提示"未消耗任何 credit"。

### 单图（`--image`）的质量建议

- **主体清晰、居中**，背景干净（推荐纯色背景）
- **光照均匀**，避免强阴影/高光/闪光灯反光
- **避免明显透视畸变**（尽量正对拍摄/正交视角）
- 主体应占据画面主要部分，不要留大片空白

### 多视图（`--images`）的强约束 ⚠️

顺序**固定为 `[front, left, back, right]`**；允许少给（至少 2 张），但 **front 必须有**。

以下几条**直接决定重建质量**，务必满足：

1. **同一物体、同一光照条件**（Tripo 官方要求）
2. **严格同一尺度、同一构图** —— 4 张图里主体的大小与画面位置应互相对应
3. 建议**裁成正方形**（1:1）
4. 4 张的**分辨率一致**
5. 各视角之间**不要有透视畸变**（正交/长焦优先，避免近距广角）

> ⚠️ **若 4 张图的尺度/位置不自洽，Tripo 重建出的几何会是扭曲的**
> （它以为这是一个不规则物体）。这是多视图链路最常见的失败原因。

### 手绘/拍照取图的额外提醒

如果参考图是**纸笔手绘后拍照**（而非数字渲染/摄影）：

- 拍照时**离远一点 + 用变焦放大**，可显著减小透视畸变
- **正对、垂直、纸面平行**拍摄，不要斜拍
- 用**均匀散射光**（窗边最好）；**不要用闪光灯**（反光 + 硬阴影会被当成表面起伏）
- 画底稿时在 4 张纸上画**对位标记**（如四角十字），拍照后按标记裁切，
  这样 4 张图能自动对齐——比"凭感觉对齐"可靠得多

### 一个已知的待验证点（alpha 通道）

带 alpha 通道的 PNG（例如透明底/半透明底稿）**能通过本地审查**，
但 **Tripo 对 alpha 的处理方式尚未在官方文档中说明**（可能保留，
也可能先把图片合成到某个背景色上）。

因此：**若使用透明底图片，建议先做一次小规模实测**再批量使用；
退路是改用**纯色底（如纯白）**。详见
`docs/jpov_gen3d_image_input_design.md` §3.6。

## 方法 B：分步（想跳过编译 / 只想生成不渲染）

各命令内部已含用法注释；想看每步产物定位用绝对路径。

```bash
# 0. 编译（一次即可，之后增量）
bazel build //tools/jpov/gen3d/static:gen3d_cmd //tools/jpov:jpov_model_viewer

# 1. 只生成+下载 GLB（stdout 最后一行就是 .glb 路径）
./bazel-bin/tools/jpov/gen3d/static/gen3d_cmd generate \
    --name <名字> --output_dir <dir> --prompt "描述"

# 2. 把上一步的 .glb 渲染成 4 视角图到 <dir>
DISPLAY=:99 ./bazel-bin/tools/jpov/jpov_model_viewer \
    --four_views --output_dir <dir> <上一步的.glb路径>
```

## 常见问题

- **生成了但下载失败？** 别重生成（会重烧 credits）。Tripo 任务会保留，
  重查 task 拿新签名 URL 再下即可 → 见 `tripo_model_download.md` §3。
- **模型是黑图/花屏？** 确认 GLB 渲染能出非纯黑图 = JPOV 加载 OK；
  纯看渲染问题往 JPOV 渲染侧查。
- **想删上一次结果？** 直接删 `output/gen3d/<名字>.glb` 和同名 4 张 png。

## 进阶文档（不用先读，需要时再翻）

- `tripo_model_download.md` — 下载模型的方法与踩坑（URL 刷新/超时/多通道）
- `docs/jpov_gen3d_design.md` — 整体架构设计（供应商无关分层、依赖、平台）
- `gen3d_cmd.cc` 头部 — 单独跑 gen3d_cmd 的参数清单
- 固定管线约束（恒 PBR/恒不透明/恒有贴图…）见 `gen3d_config.h` 顶部

# gen3d 使用说明（从这句开始读）

> 面向第一次接触本文件夹、想"生成一个静态 3D 模型并看到它的图"的人。
> 按下面步骤照做即可出图，**不需要先读源码**。想看内部原理再翻文末"进阶文档"。

## 本文件夹是什么

`gen3d` 是 JPOV 的工具：给一段文字（prompt），自动去 Tripo 生成一个
**静态 PBR 的 3D 模型文件（.glb）**，再用 JPOV 渲染成 4 张预览图给你看。
它**必须配合本仓库（ai_infra/ai_infra_2）的代码和 bazel 才能用**，不能脱离仓库独立运行。

```
文字描述 ──> gen3d ──> 模型文件 .glb + 4 张预览图 .png
```

## 前置条件（先确认，缺一不可）

1. 在 JPOV 仓库根目录（`/james_pm/ai_infra_2`）下操作。
2. 环境变量 `TRIPO_API_KEY` 已设置（调 Tripo 需 key，会烧 credits）。
   检查：`echo $TRIPO_API_KEY` 非空即可。没设先去设。
3. 有 bazel；渲染需要显示（无 DISPLAY 会自动起 Xvfb，见 gen3d.sh）。

## 方法 A：一键全链（推荐，只跑这一条命令）

在仓库根目录执行：

```bash
bash tools/jpov/gen3d.sh <output_dir> <名字> --prompt "你想生成的模型的描述"
```

例：

```bash
bash tools/jpov/gen3d.sh output/gen3d indoor_column \
    --prompt "浅白色大理石柱，圆柱主体，上下有方形基座"
```

做了什么：

1. 自动 bazel 编译 gen3d_cmd + model_viewer
2. 调 Tripo 按 prompt 生成模型 → 下载到 `output/gen3d/indoor_column.glb`
3. 用 JPOV 渲染 4 视角 → `indoor_column_{front,up,left,perspective}.png`
4. 最后打印**全部产物绝对路径**，照清单去拿。

**常用可选参数**（追加在 --prompt 后面）：
| 参数 | 作用 |
|---|---|
| `--triangles <n>` | 面数预算，默认约 4000；范围 50~20000 |
| `--real_size` | 按真实物理尺寸（米）输出（如"柱子高5米"会生效） |
| `--negative "..."` | 排除不想出现的内容 |
| `--high_poly` | 关掉低模默认（本次仍映射同档，H 档为后续扩展） |

跑完想验证模型真的好 → 直接看 4 张 PNG；或 `file output/gen3d/<名字>.glb`
应显示 `glTF binary model, version 2`。

## 方法 B：分步（想跳过编译 / 只想生成不渲染）

各命令内部已含用法注释；想看每步产物定位用绝对路径。

```bash
# 0. 编译（一次即可，之后增量）
bazel build //tools/jpov/gen3d:gen3d_cmd //tools/jpov:jpov_model_viewer

# 1. 只生成+下载 GLB（stdout 最后一行就是 .glb 路径）
./bazel-bin/tools/jpov/gen3d/gen3d_cmd generate \
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

# gen3d/skeleton 使用说明

> gen3d 顶层的**带骨骼**部分：给一段文字（prompt），自动去 Tripo 生成**带 mixamo 兼容骨骼
> （mixamorig）的人形/生物 .glb**。纯静态 PBR 生见 sibling `gen3d/static/`。

### 能干什么
Tripo v3 两步链（见下方）产出一**带 23 关节 mixamorig 骨架 + JOINTS_0/WEIGHTS_0 蒙皮权重 + IBM**、
**无预烘焙动画帧**的 GLB —— 这是"先拿骨架、后由 loader/skeleton 系统驱动动画"路线的基础资材。

```
文字 ──> gen3d_skeleton ──> 带骨骼(bind pose) .glb
```

### 依赖 / 前置
1. 在仓库根 `/james_pm/ai_infra_2` 操作；有 bazel。
2. 环境变量 `TRIPO_API_KEY` 已设（会烧 credits：text-to-model + rig）。

### 一键使用（推荐）
```bash
bash tools/jpov/gen3d_skeleton.sh <output_dir> <name> --prompt "角色描述"
# 例:
bash tools/jpov/gen3d_skeleton.sh output/gen3d_skeleton man \
    --prompt "bare-chested athletic male, wearing underwear shorts, bald head, \
    neutral standing pose, clearly defined torso muscle and limb joints, riggable biped"
```
产物 = `<output_dir>/<name>.glb`（mixamorig 带骨骼）。命令会实时打印 task_id / 任务查询 URL /
签名下载 URL —— **若下载失败可用它们手动抢救**（curl 签名 model_url，或 GET /tasks/{id} 重看状态）。

### 分步（想跳过文本生成 / 只 rig 已有任务）
```bash
# 0. 编译
bazel build //tools/jpov/gen3d/skeleton:gen3d_skeleton_cmd

# 1. 默认：prompt → text-to-model → Auto Rig(mixamo)
./bazel-bin/tools/jpov/gen3d/skeleton/gen3d_skeleton_cmd generate \
    --name <slug> --output_dir <dir> --prompt "..."

# 2. 已有模型 task_id → 只 rig（跳过 text-to-model，省一次生成钱）
./bazel-bin/tools/jpov/gen3d/skeleton/gen3d_skeleton_cmd generate \
    --name <slug> --output_dir <dir> --input_task_id <task_xxx> \
    [--spec mixamo|tripo] [--rig_type biped|...]
```
可选参数：`--negative "..."`（text 阶段排除词）、`--spec`（默认 mixamo）、`--rig_type`（默认 biped）。

### 产物结构（check 用）
已验证 `mixamo_male.glb`：1 skin / 23 joints / 节点全 `mixamorig:` 前缀 / 顶点含 JOINTS_0+WEIGHTS_0 /
IBM 在位 / animations=0。用 python tinygltf 或 glb JSON chunk 可复查，勿凭猜。

### Tripo v3 契约（真调证实）
- 步1 `POST /v3/generation/text-to-model`；步2 Auto Rig `POST /v3/animations/rig`
  body `{input:<上步 task_id>, model, rig_type, spec:"mixamo", out_format:"glb"}`
- biped → rig model `v1.0-20240301`；其余 creature → `v2.5-20260210`
- 编辑类任务（texture/decimate/convert quad）会**剥 skin** → 先做完网格再 rig/retarget

### 说明
渲染侧暂不驱动骨骼（loader 未接 skin）。本目录先把"产出带骨骼 GLB"跑通；驱动/渲染验证在下个 PR。
架构见 `docs/jpov_clothes_rig_design.md`、`docs/jpov_gen3d_design.md`。

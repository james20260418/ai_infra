# Tripo 模型下载说明（gen3d 的一部分）

> 用途：这是 `gen3d` 工具里「生成静态 3D 模型 GLB」这一段的**实战说明**，
> 记录 Tripo API 从提交任务到下载模型文件的正确姿势 + 踩过的坑。
> 它不是独立 skill——强烈依赖本仓库 gen3d_cmd / tripo_client / jpov 渲染链，
> 离开代码仓无法单独使用。跟走代码，看文档前先读 `gen3d_config.h` 顶部
> 的固定管线约束与 `docs/jpov_gen3d_design.md`。

## 一、整条链路顺序（谁调谁）

```
gen3d.sh <output_dir> <name> --prompt "..."
  │── bazel build gen3d_cmd + jpov_model_viewer
  │── gen3d_cmd generate          ← 提交 → 轮询 → 下载 GLB（本说明重点）
  │       （Tripo REST, 见下节）
  └── jpov_model_viewer --four_views --output_dir   ← 渲染 4 视角 PNG
```

分工：`gen3d_cmd` 只管"文本 → 生成 → 把 GLB 下到本地"；截图交给
`jpov_model_viewer`。本说明只讲 gen3d_cmd 的生成+下载这一段。

## 二、Tripo API 调用方式（gen3d_cmd 内部）

- 端点：只用 **V3**（V2 已于 2026-11-01 退役）。
  基址 `https://openapi.tripo3d.ai/v3`（全球 .ai，本项目默认）。
- 认证：`Authorization: Bearer <api_key>`，key 读环境变量 `TRIPO_API_KEY`。
  **key 不进命令行参数**（防 shell 历史泄漏）。
- 流程：
  1. `POST /v3/generation/text-to-model`
     body `{"prompt","model":"P1-20260311","face_limit","texture":true,
            "pbr":true,"export_uv":true,"texture_quality","auto_size"}`。
     立即返回 `{"code":0,"data":{"task_id"}}`。（低面 50~20000，见 client 内 clamp）
  2. 轮询 `GET /v3/tasks/{task_id}`，每 ~2s，直到 `status=success`。
     典型 10~120s。
  3. `success` 时从响应 `data.output.model_url` 下载 GLB。
     下载的签名 URL **不带 Authorization 头**。
- tasks 查询返回结构（download 依据）：
  ```
  data.status / data.output.model_url / data.output.rendered_image_url
  ```

## 三、下载模型的关键（重点踩坑区）

### 3.1 下载 URL 的获取
- **永远以 task 响应实际返回的 `model_url` 为准**，不要信二手文档/站外
  搜到的域名。Tripo 真实模型下载域是
  `https://tripo-data.rg1.data.tripo3d.com/...`（**不是**站上展示的 cdn/
  cloudflare 域，那个多半是预览图或无关）。
- 下载 URL 是**带签名、5 分钟有效**的临时地址，返回里含
  `Policy / Signature / Key-Pair-Id` 等参数。过期就失效。

### 3.2 URL 过期怎么办 —— 重新查询刷新（不重烧 credits）
- 这最重要：**模型生成就会计费**（success 即扣 credit，与你下没下载无关）。
  若因 URL 过期/慢导致下载失败，**绝不重新生成**（那会再烧 credits）。
- 正确做法：重新 `GET /v3/tasks/{task_id}` → 会返回**一份新签名的
  model_url** → 立刻下载。task 记录在服务端保留，总能刷到新 URL。
- 结论：下载环节失败 =「重新查 task 刷新 URL 再下」，不是「重新生成」。

### 3.3 下载超时 vs 下载失败（别把 timeout 当网络不通）
- **下载要独立的、更长的超时**（本项目默认 300s），别跟 JSON API 请求
  （60s）共用。大模型/慢链路下 60s 可能下到一半被掐。
- 判断法：同一 URL 用 `curl CLI` 能秒下、自己程序却 timeout → 99% 是
  程序 timeout 设太紧，先查自己代码，别急着归咎网络。
- 本项目历史：早期共用 60s 导致 GLB 下到一半 `Timeout was reached`
  且残留半成品文件 → 误判成"CDN 被墙网络无解"。真实是超时口没给够。

### 3.4 半成品/失败文件处理
- 下载到一半失败会留**残缺文件**（有大小但内容不完整），会伪装成"已生成"。
- 必须**边下边写临时文件，失败即删残留 + 明确报错**，别把残 GLB 当成果。
- 落盘后建议用 `file` 看 `glTF binary model, version 2` 或直接进
  JPOV `LoadGltf` 渲染，确认是真 GLB。

### 3.5 多个通道的意识（不要锁死单一域）
- 若默认下载域不稳，Tripo 数据理论上也能从其他可达端取（今日实测 `.com`
  系 `openapi.cdn.tripo3d.com` 也有样例）。但**真实返回哪个就用哪个**，
  用真实 URL 试，别硬编码/猜域。
- 判定某域通不通，必须先拿到真实返回 URL 去探测，不能凭站点文档猜。

## 四、工具链使用速查（在 ai_infra_2 下）

```bash
# 直接调 gen3d_cmd（只生成+下载，不出图）
./bazel-bin/tools/jpov/gen3d/gen3d_cmd generate \
    --name indoor_column --output_dir output/gen3d \
    --prompt "浅白大理石圆立柱配方形基座" --triangles 3000 --real_size

# 全链一次跑（生成+下载+JPOV 渲染 4 视图+打印产物清单）
bash tools/jpov/gen3d.sh output/gen3d indoor_column --prompt "……"

# 手动验证下载/刷新生效（不重生成，用旧 task_id）
TASK=某task_id
URL=$(curl -s https://openapi.tripo3d.ai/v3/tasks/$TASK \
  -H "Authorization: Bearer $TRIPO_API_KEY" | python3 -c \
  "import sys,json;print(json.load(sys.stdin)['data']['output']['model_url'])")
curl -f -L "$URL" -o model.glb
```

## 五、一次成功下载的判定
- 下载 200 + 文件能被 JPOV 加载渲染出**非黑图**的多视角图 = 真成功。
- 别只看体积（有纹理 PBR 与无纹理体积差异很大）。

> 本文随 gen3d 工具版本管理；改下载逻辑请同步更新 §三。

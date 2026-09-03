# JPOV gen3d — 自动生成静态 3D 模型工具（设计）

> 目的：让 JPOV 具备「文本 prompt → 生成静态 PBR GLB → headless 渲染验证」的自闭链路。
> 对应 2026-08-24 调研 + PR #77 落下的 gen3d_config.h 骨架。本文是本次 PR 的功能设计。

## 0. 现状（已核实）

- `tools/jpov/gen3d/gen3d_config.h` 已存在：供应商无关的 `Gen3dConfig`（prompt / negative_prompt / low_poly / real_size / max_triangles）+ 固定管线约束注释。
- **没有 Tripo client、没有 HTTP 依赖、没有 gen3d BUILD** —— 本次 PR 的核心空白。
- `demo/jpov_model_viewer.cc` 已有 `--four_views` headless 拍照（4 张 PGN：front/up/left/perspective），但**输出目录写死为 glTF 所在目录**，缺 `--output_dir`。
- 仓库第三库集成模式：`third_party/<name>-static/`（.a + include + BUILD + linkopts），有 glfw-static / glog-static / opencv-static 先例。
- JPOV 铁律：Windows 交叉编译（MinGW）+ Linux 两条链都要能构建（CI test + windows-build 双 job）。

## 1. 目标架构（本次 PR 范围）

```
gen3d.sh <output_dir> <prompt...>
  │  （顶层 bash 脚本，编排「编译 + 多次 elf 调用」，不会自己发 HTTP）
  │
  ├─ 步骤0: bazel build //tools/jpov:gen3d_cmd          ← 新 CLI（调 Tripo API）
  │         bazel build //tools/jpov:jpov_model_viewer    ← 已有（加 output_dir）
  │
  ├─ 步骤1: gen3d_cmd generate
  │           --output_dir <dir>  --name <slug>
  │           --prompt "..." [--negative "..." --low_poly --triangles 4000]
  │           内部：Tripo V3 提交 → 轮询 → 下载 GLB 到 <dir>/<name>.glb
  │           自己在 stdout 打印最终产物绝对路径（防 confuse）
  │
  └─ 步骤2: jpov_model_viewer --four_views --output_dir <dir> <dir>/<name>.glb
             → 输出 <dir>/<name>_front/up/left/perspective.png
             （加 --output_dir 后不再写 glTF 同级目录）
```

范围边界：**本次 PR 只做 Linux 一条链 + Tripo 一个供应商**，把"自动生成静态模型"跑通闭环。Windows 交叉编译不引入 HTTP 依赖（见 §5），Model viewer 的 output_dir 改动需双平台均编译通过但出图仅 Linux 用。

## 2. 依赖：libcurl（静态，vendoring）

### 2.1 为什么 http 依赖是必要且最重的一环
Tripo 是纯 REST + HTTPS。C++ 里要：
- `POST https://openapi.tripo3d.ai/v3/generation/text-to-model`（含 JSON body + Bearer 头）
- `GET /v3/tasks/{id}` 轮询
- 从 `output.model_url` 下载 GLB（签名 URL，**不带** Authorization 头，5 分钟过期）

需要一个同时支持 HTTPS(TLS) + 直连下载文件 + 请求头定制的客户端 → **libcurl**。

### 2.2 集成方式：源码裁剪版静态 libcurl（不入第三方包管理器）
系统 apt 的 `libcurl.a` **不可直接 vendoring**（实测缺陷）：
- 静态链接报 `gss_*` / nghttp2 / psl / brotli / zstd 等一路符号地狱（Ubuntu 编译时特性全开）
- 不可复现：依赖某台机的 apt 版本

因此参照 `third_party/glfw-static` 模式，**自己从 curl 源码 configure 裁剪**：
- `--disable-*` 全部非必需特性：`--disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-unix-sockets`
- **TLS 后端**：`--with-openssl`（OpenSSL 静态 vendoring，另编译 libssl.a + libcrypto.a）
- 保留：HTTP/HTTPS/file + json（无内置 json，JSON 正文手拼或单独引入）
- linkopts：`-lssl -lcrypto -lz -lpthread -ldl`（缺哪个补哪个）
- 产出：`third_party/curl-static/` 下含 `libcurl.a` + include/curl/*.h + BUILD

### 2.3 libcurl 目标平台
- **Linux：必须**（本次跑通链路）
- **Windows(MinGW)：本次不引入**（见 §5 兼容性说明）——Windows 上 gen3d 链路不构建，仅 model viewer 的 output_dir 改动两平台都过。

## 3. JSON 处理
Tripo 请求/响应 JSON 结构嵌套且要容错（code/data/status/output.model_url），手写解析易错且测试成本高。**引入单头 `nlohmann/json`**（v3.11.3，header-only 无依赖）到 `third_party/nlohmann-json/`，成熟稳定，bazel 集成极简（hdrs 即可）。

## 3b. Tripo V3 映射（已核实 developers.tripo3d.ai 权威参数）

只用 V3 通用端点（V2 已 2026-11-01 退役），base `https://openapi.tripo3d.ai/v3`：
- 提交 `POST /generation/text-to-model`，body：
  ```json
  {"prompt", "model":"P1-20260311", "face_limit":3000,
   "texture":true, "pbr":true, "texture_quality":"detailed",
   "negative_prompt":"...", "auto_size":false, "export_uv":true}
  ```
  → `{"code":0, "data":{"task_id":"task_xxx"}}`
- 轮询 `GET /tasks/{task_id}` 每 ~2s → `status:success` 时 `data.output.model_url`（GLB 签名单）
- 下载 model_url（**不带** Authorization 头，5 分钟过期）→ 落盘 `output_dir/name.glb`
- Gen3dConfig 映射：`low_poly=true`→`model=P1-20260311`（低模主力）+ `face_limit=max_triangles`；
  P1 面数范围 50~20000（REC 简单≥150 复杂≥250）。texture/pbr 由固定管线约束恒 true。
- 注：中文站 docs.tripo3d.ai 用 `model_version`/`type` 命名，是另一套；
  **以 developers 站 V3 端点 `model` 字段为准**（与 SDK/quick-start 一致）。

## 4. gen3d_cmd 接口（新 bin）

单个 Linux ELF，第一参数子命令：

```
gen3d_cmd generate \
    --name <slug>            # 产物 basename（.glb），必填
    --output_dir <dir>       # 产物父目录，必填
    --prompt "<...>"         # 必填
    [--negative "<...>"]     # 可选
    [--low_poly]             # 低模模式
    [--triangles <n>]        # 面数预算（low_poly 时默认 4000；否则默认 100000）
```

行为：
1. 读环境变量 `TRIPO_API_KEY`（校验非空，缺失即报错退出）
2. 拼 `Gen3dConfig` → 映射到参数结构（Tripo 供应商字段在 client 内私有）
3. `POST text-to-model`，取 task_id
4. 每 2s `GET tasks/{id}` 轮询，直到 success / failed / timeout(120s)
5. success 后立即从 `output.model_url` 下载到 `<output_dir>/<name>.glb`（mkdir -p 自动建目录）
6. stdout 打印最终 `.glb` 绝对路径 + 退出码 0
   - 失败：打印任务状态/错误，非零退出

class 划分（供应商无关设计延续 gen3d_config.h）：
- `TripoClient`：唯一直接发 HTTP/解析 Tripo 响应的类。持有 base_url + api_key。
- `TripoRequestConfig` / 透传 `Gen3dConfig` → Tripo 参数映射（smart_low_poly → model/face_limit 等）在 client 内部。
- main() 只做 CLI 解析 + 调 generate 逻辑。

## 5. 平台兼容性决策（重点，需对齐）

| 组件 | Linux | Windows(MinGW) |
|---|---|---|
| gen3d_cmd + libcurl | ✅ 构建运行 | ❌ 不建（BUILD target_compatible_with 限 linux；避免交叉编译 curl 的复杂度） |
| jpov_model_viewer `--output_dir` | ✅ | ✅（仅加参数，出图仍 Linux 用） |

理由：本次目标是"Linux 下跑通自动生成闭环"。Windows 引入 libcurl/OpenSSL 交叉静态编译会显著扩大 PR 面（MinGW 下 OpenSSL 依赖链复杂）。为此：
- gen3d_cmd 的 cc_binary 标 `target_compatible_with = linux_x86_64`（与 jpov_model_viewer 同款），Windows 配置天然跳过。
- CI windows-build job 不受影响（不打 gen3d 的 windows target）。
- Model viewer 加 `--output_dir` 需两平台编译通过 → 这项改动要 windows 也过。

「Windows 也要 gen3d」作为后续增强，不再本次。

## 6. gen3d.sh（顶层编排 bash）
见目标架构。职责边界：
- 编译两个 bin（若 output/ 下有现成产物可跳过可选 --no-build）
- 调 gen3d_cmd → 拿 .glb 路径
- 调 jpov_model_viewer --four_views --output_dir → 拿 4 张 png
- **结束打印产物清单**（glb 路径 + 4 png 路径），打印 output_dir 方便用户直取

## 7. 现场验证（Danis 提供 prompt 后）
链路跑通判据：
- gen3d_cmd 真实提交 + 轮询 + 成功下载有效 GLB（能进 JPOV LoadGltf）
- model viewer four_views + output_dir 出 4 张图到指定目录
- `.glb` 能正常渲染（无黑模/破面）

## 8. 本次 PR 文件清单（预估）
```
third_party/curl-static/…       裁剪版静态 curl（.a+include+BUILD+README 注明编译命令）
third_party/openssl-static/…    openssl 静态（若需独立 vendoring）
tools/jpov/gen3d/BUILD          新增（gen3d 库，供 demo 挂载）
tools/jpov/gen3d/tripo_client.h / tripo_client.cc
tools/jpov/gen3d/gen3d_config.h (已存在，延续)
third_party/curl-static/       裁剪静态 curl+openssl+z（BUILD+README+配方）
third_party/nlohmann-json/     nlohmann/json 单头

tools/jpov/demo/gen3d_cmd.cc    新 CLI main
tools/jpov/demo/jpov_model_viewer.cc  加 --output_dir
tools/jpov/BUILD                加 gen3d_cmd bin + deps
tools/jpov/build_gen3d.sh       新编排脚本
tools/jpov/docs/jpov_gen3d_design.md (本文)
```

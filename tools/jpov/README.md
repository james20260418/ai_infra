# JPOV — 面向 AI 自测的轻量可视化沙盒

JPOV 是一个面向 AI 自测输出的、支持流式绘制 + 简单交互的轻量可视化沙盒。详见 [DESIGN.md](DESIGN.md)。

> ⚠️ **改 / 用 JPOV 之前先读这些（防止踩已有结论的坑）**
>
> - **[docs/jpov_engine_integration.md](docs/jpov_engine_integration.md)** — ★必读。用 JPOV 搭游戏引擎 / 当「0 级引擎」时的定位、线程模型（GL 单线程铁律）、static meshing 可行、局部更新/分层**明确不做**。任何想把 JPOV 当游戏引擎基石的 agent，先读它。
> - **[docs/jpov_crowd_instancing_arch.md](docs/jpov_crowd_instancing_arch.md)** — 城市级人群批量实例化（千人 instancing + charactor-part selector）的架构锚点：肉体/衣物归一为共享骨架 rest-mesh、肤色/装备廉价切换走 select 共享区、肢体到小臂/大臂/头部位、S0 阶梯。engine-integration 的城市场景续集，做人群前先读。
> - **[DESIGN.md](DESIGN.md)** — 设计文档（注意其中早期 API 描述部分与代码现状不一致，以代码为准，见集成文档 §8）。
> - **[docs/jpov_effect_pass_design.md](docs/jpov_effect_pass_design.md)** — 粒子特效 pass 设计（火焰 MVP）：并列 primitive3d 的特效通道、简单深度（测而不写）、additive + alpha 两种混合、无 instancing、「照抄式复用」+ 薄公共层策略。做火焰/烟/雨/雪前先读。
> - **[docs/jpov_volumetric_fog_design.md](docs/jpov_volumetric_fog_design.md)** — 局部体积雾（球形/圆柱）设计：像点光源一样摆放雾体、**闭式积分**（零采样零噪声）、**tile culling 限流**（每像素 overdraw 硬上限 K）、一次全屏 pass 累加 τ/S + 就地合成、HDR/tone map 之前。做局部雾/烟尘/发光体空气光前先读。
> - **[docs/jpov_instance_attr_lag_design.md](docs/jpov_instance_attr_lag_design.md)** — 人物 instancing per-instance attribute 布局重排 + 布料 LAG（惯性滞后副运动）：双 pose + 双 partial + 逐顶点松弛度 mix；三视图约定（一槽=4 float/8 short/16 int8）；16 槽（loc0..15）定稿布局。做副运动/重排 attribute 前先读。
> - **[docs/jpov_wbo_target.md](docs/jpov_wbo_target.md)** — WBO 目标（遮掩/发光两集合）的共享件：`WboTarget`（固定槽位、附件按需申请、clear/blend/还原一把抓）+ 共享权重与解析 + 新加 WBO 层必须遵守的 7 条规则。写任何半透明累积层前先读。
> - **[docs/jpov_strip3d_translucent_design.md](docs/jpov_strip3d_translucent_design.md)** — 半透明 3D 条带（Strip3D）：左右双轨 + 逐点 uv/alpha + 8 段宽度剖面 → 1× WBO 双集合（遮罩 / 发光）。廉价玻璃、流星拖尾、剑气、光环；不动三大 renderer 的 blend。做带状半透明效果前先读。
> - **[interface/README.md](interface/README.md)** — 渲染接口层说明。
> - **[tools/vegetation_tool_v2/README.md](tools/vegetation_tool_v2/README.md)** — 植被资产链（Blender/Sapling → shape）。**资产一律存二进制 `.vegshape`**（ShapeProto 的 protobuf 二进制），文本 `.pbtxt` 只用于人工查看，用 `gen/shape_convert` 互转；含三棵树的三档顶点预算（5 万顶点 / 5000 三角 / 高配）。

## 快速开始

### 一键构建（双平台）

```bash
./tools/jpov/build_jpov_demo.sh
```

产物输出到 `output/jpov_demo/`：
- `jpov_demo` — Linux ELF（在 WSL2 中运行，需要 DISPLAY）
- `jpov_demo.exe` — Windows PE（静态链接，直接拷贝到 Windows 后双击运行）

### 单平台构建

**Linux：**
```bash
bazel run //tools/jpov:jpov_demo
```

**Windows（交叉编译）：**
```bash
bazel build //tools/jpov:jpov_demo.exe --config=windows
# 产物在 bazel-bin/tools/jpov/jpov_demo.exe
```

## 平台支持

| 平台 | 状态 | 说明 |
|------|------|------|
| Linux x86_64 | ✅ | 完整功能，含 OpenCV 截图 |
| Windows x86_64（MinGW 交叉编译） | ✅ | 窗口展示 + 交互，无 OpenCV 截图 |

Windows 版缺少 OpenCV 截图功能（需要 MinGW 交叉编译 OpenCV 静态库），其余窗口绘制、输入响应、blend 等功能与 Linux 版一致。

## 渲染设备偏好（prefer GPU / prefer CPU）

`JPOV::Config::render_preference`（默认 `kAny` = 完全不介入，行为与老版本一致）：

| 值 | 建 context **之前** | 建 context **之后** |
|----|--------------------|--------------------|
| `kAny` | — | — |
| `kPreferGpu` | Windows exe 导出表**恒**含 `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`（混合显卡笔记本优先独显） | 落到已知软件光栅器（llvmpipe / SwiftShader / GDI Generic / WARP…）→ `WARNING` + 排查清单 |
| `kPreferCpu` | 对 Mesa 系驱动设 `LIBGL_ALWAYS_SOFTWARE=1`（**已显式设置则不覆盖**） | 落到 `kHardware` → `WARNING` |

**这不是「选显卡」**：Windows/Linux + OpenGL 都没有这个能力（见 [interface/gpu_info.h](interface/gpu_info.h)
顶部注释）——最终落到哪块设备由驱动 / 系统「显示设置 → 图形」的 per-app 首选项 / X server 决定。
偏好只能「提个醒 + 事后核对 + 把怎么办写进日志」，判定表见 [interface/render_preference.h](interface/render_preference.h)。

跑起来后看启动日志那一行即可：

```
GL: hardware | renderer=NVIDIA GeForce RTX … | vendor=NVIDIA Corporation | … | preference=prefer-gpu
```

或按 `F3` 打开 debug HUD（左上角显示 `gpu:` 一行）。

## Windows 分发：代码签名 / SmartScreen（谁负责什么）

**证书归发布者，工具归引擎。** 私钥一旦进了引擎仓库就等于公开（任何人都能签出
"官方"exe），而且签名表达的是「谁发布」，所以：

- **JPOV 提供工具**：`tools/jpov/sign_windows_exe.sh`（构建脚本已自动调用）——
  生成自签名代码签名证书（10 年，`output/signing/`，私钥不进 git）→ 用
  `osslsigncode` 签名并校验 → 往产物目录放 `jpov_codesign.crt` +
  `install_cert.bat` / `uninstall_cert.bat`（一键装进「受信任的根证书颁发机构 +
  受信任的发布者」，需管理员，只需一次）。
- **发布者自己持有证书**：默认 CN 是 `JPOV Local Dev`（开发机够用），产品或团队
  发布时用 `JPOV_SIGN_CN="Studio XYZ"` 覆盖；**不要**把某个人的证书/私钥提交进仓库。

边界（别指望它解决一切）：

| 场景 | 自签名证书够吗 | 说明 |
|------|---------------|------|
| 自己的开发机 / 内网工具 | ✅ | 装一次证书即可，不再弹「未查到此应用的发布信息」 |
| 发给同事的机器 | ✅ | 对方跑一次 `install_cert.bat` |
| 对外公开发布 | ❌ | 需要 CA 代码签名证书（OV/EV）或 MSIX / 商店分发；自签名在别人机器上仍是未知发布者 |

另外：从网络复制来的 exe 带 **MOTW**（网络来源标记），即使签名也可能被拦 ——
右键属性勾「解除锁定」或 `Unblock-File -Path <exe>`。

## 运行日志（Windows 默认落盘）

双击启动的进程没有可看的控制台（已用 `-mwindows` 做成 GUI 子系统），所以
`JPOV::Init()` 一开始就把 stderr 重定向到文件：

| `Config::log_file` | 行为 |
|--------------------|------|
| `nullptr`（默认） | **自动**：Windows → exe 同级 `jpov_YYYYMMDD-HHMMSS_<pid>_log.txt`；其他平台不落盘 |
| `""` | 彻底关闭日志 |
| 其他路径 | 写到该路径（相对路径按 **exe 目录**解析） |

- 截断写 + 无缓冲：`LOG(FATAL)` / 崩溃前的最后一行也一定在盘上。
- exe 目录不可写（例如直接跑在 `\\wsl$` 共享上）→ 自动退到 `%TEMP%\jpov_run_log.txt`，
  日志第一行会打印真实路径。
- Windows 另外装了 `SetUnhandledExceptionFilter`：访问违例这类崩溃会写异常码 / RIP /
  exe 内 rva / 栈扫描结果（没有符号表时定位调用点的唯一办法）。

## 各 viewer 的 Windows 构建现状

所有交互式 viewer 都有 `.exe` target（MinGW 交叉编译、静态链接，可直接拷到 Windows 上跑，
运行时所需的 `fonts/` `models/` `textures/` 由对应脚本一并拷到 exe 旁）。

构建脚本统一接受 `--windows`：**默认只出 Linux ELF**，加 `--windows` 才额外交叉编译 exe
（Windows 那一趟要多花几十秒，不需要时不必等）。只有
`build_jpov_demo.sh` / `build_jpov_ui_demo.sh` / `build_jpov_sp_ui_demo.sh` /
`build_jpov_skylight_viewer.sh` 四个老脚本本来就是双出，保持原样（加不加 `--windows` 都一样）。

| target | 构建脚本 |
|--------|---------|
| `//tools/jpov:jpov_demo.exe` | `build_jpov_demo.sh`（双出） |
| `//tools/jpov:jpov_ui_demo.exe` | `build_jpov_ui_demo.sh`（双出） |
| `//tools/jpov:jpov_sp_ui_demo.exe` | `build_jpov_sp_ui_demo.sh`（双出） |
| `//tools/jpov:jpov_skylight_viewer.exe` | `build_jpov_skylight_viewer.sh`（双出） |
| `//tools/jpov:jpov_model_viewer.exe` | `build_jpov_model_viewer.sh --windows` |
| `//tools/jpov:jpov_pick_highlight_viewer.exe` | `build_jpov_pick_highlight_viewer.sh --windows` |
| `//tools/jpov:jpov_fog_viewer.exe` | `build_jpov_fog_viewer.sh --windows` |
| `//tools/jpov:jpov_soft_mesh_viewer.exe` | `build_soft_mesh_simulator.sh --windows` |
| `//tools/jpov:jpov_particle_viewer.exe` | `build_particle_viewer.sh --windows` |
| `//tools/jpov:jpov_volume_fire_viewer.exe` | `build_volume_fire_viewer.sh --windows` |
| `//tools/jpov:jpov_strip_viewer.exe` | `build_jpov_strip_viewer.sh --windows` |
| `//tools/jpov/demo/fbx_viewer:jpov_fbx_viewer.exe` | `build_jpov_fbx_viewer.sh --windows` |
| `//tools/jpov/tools/vegetation_tool_v2:jpov_vegetation_v2_viewer.exe` | `build_jpov_vegetation_v2_viewer.sh --windows` |

这 13 个 exe 全部列进了 CI 的 `windows-build` job —— 没人交叉编译的 target 不会自己保持可编译。

还没做 exe 的：`jpov_clothing_tool`、`jpov_model_editor`（编辑器/工具链未做交叉编译验证）。
要加一个：照 BUILD 里现有 `.exe` target 复制一份，只把 `target_compatible_with` 换成
`@platforms//os:windows`，跑 `bazel build //tools/jpov:<name>.exe --config=windows`，
能过就补进 CI 的 windows-build job。

### MinGW 宏坑（都在公共头里一次性处理了）

- `windows.h` 把 `near` / `far` 定义成空宏，而 `Camera` 的字段就叫 `near`/`far`
  ⇒ `cmds.camera.near = 0.1f` 展开成 `cmds.camera. = 0.1f`。`jpov.h` 与
  `tools/common/utils.h`（两者都可能最后被 include）分别 `#undef` 一次。
- `windows.h` 把 `DrawText` 定义成 `DrawTextA`，会劫持 `cmds.DrawText(...)`
  ⇒ 链接期找不到 `RenderCommandList::DrawTextA`。同样在 `utils.h` 里撤销。
- MinGW + `-std=c++20` 打开 `__STRICT_ANSI__`，`M_PI` 只有在包含 `<cmath>` **之前**
  定义 `_USE_MATH_DEFINES` 才可见。`.bazelrc` 的 `build:windows` 统一 `-D_USE_MATH_DEFINES`。

## 渲染设备偏好（prefer GPU / CPU）

交互式 viewer 在**窗口模式**统一走 `jpov_viewer::PreferGpuForInteractiveViewer(cfg)`
（`demo/viewer_render_preference.h`），即 `RenderPreference::kPreferGpu`；
`--capture` / `--four_views` / `--ui_shot` 等 headless 路径保持默认 `kAny`
（CI 与黄金图本来就跑 llvmpipe，刷「偏好落空」告警只是噪音）。

覆盖到的 16 个入口：13 个有 exe 的交互式 viewer + `jpov_polyline_demo`
+ 两个 GUI 工具（`jpov_clothing_tool` / `jpov_model_editor`）。纯 headless 的自检工具
（`jpov_screenshot_demo` / `jpov_text_demo` / `jpov_polyline_verify` /
`jpov_model_viewer_smoke`）与 gold generator 不设，保持 `kAny`。

注意 JPOV **不能选显卡**：这个偏好只做「建 context 前的驱动提示 + 建 context 后用
`glGetString` 核对 + 落空时告警」三件事，最终设备仍由驱动 / 系统 per-app 设置决定。
语义与判定表见 `interface/render_preference.h`。

## 设计

详见 [DESIGN.md](DESIGN.md)。

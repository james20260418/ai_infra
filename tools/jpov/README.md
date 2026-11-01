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

## 各 viewer 的 Windows 构建现状

目前已构建出 `.exe` 的（可直接拷到 Windows 跑）：

| target | 构建脚本 |
|--------|---------|
| `//tools/jpov:jpov_demo.exe` | `build_jpov_demo.sh` |
| `//tools/jpov:jpov_ui_demo.exe` | `build_jpov_ui_demo.sh` |
| `//tools/jpov:jpov_sp_ui_demo.exe` | `build_jpov_sp_ui_demo.sh` |
| `//tools/jpov:jpov_skylight_viewer.exe` | `build_jpov_skylight_viewer.sh` |

其余 viewer / 工具目前**只有 Linux target**（`jpov_pick_highlight_viewer`、`jpov_fire_fog_viewer`、
`jpov_model_viewer`、`jpov_particle_viewer`、`jpov_strip_viewer`、`jpov_volume_fire_viewer`、
`jpov_soft_mesh_viewer`、`jpov_clothing_tool`、`jpov_fbx_viewer`、`jpov_model_editor`、
vegetation v2 viewer）。要加一个：照 BUILD 里现有 `.exe` target 复制一份，只把
`target_compatible_with` 换成 `@platforms//os:windows`，再跑
`bazel build //tools/jpov:<name>.exe --config=windows` —— 能过就补进 CI 的 windows-build job，
否则会悄悄腐坏（没人交叉编译的 target 不会自己保持可编译）。

## 设计

详见 [DESIGN.md](DESIGN.md)。

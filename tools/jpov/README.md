# JPOV — 面向 AI 自测的轻量可视化沙盒

JPOV 是一个面向 AI 自测输出的、支持流式绘制 + 简单交互的轻量可视化沙盒。详见 [DESIGN.md](DESIGN.md)。

> ⚠️ **改 / 用 JPOV 之前先读这些（防止踩已有结论的坑）**
>
> - **[docs/jpov_engine_integration.md](docs/jpov_engine_integration.md)** — ★必读。用 JPOV 搭游戏引擎 / 当「0 级引擎」时的定位、线程模型（GL 单线程铁律）、static meshing 可行、局部更新/分层**明确不做**。任何想把 JPOV 当游戏引擎基石的 agent，先读它。
> - **[DESIGN.md](DESIGN.md)** — 设计文档（注意其中早期 API 描述部分与代码现状不一致，以代码为准，见集成文档 §8）。
> - **[interface/README.md](interface/README.md)** — 渲染接口层说明。

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

## 设计

详见 [DESIGN.md](DESIGN.md)。

# JPOV — 面向 AI 的可视化沙盒设计文档

> 日期：2026-04-26
> 状态：设计敲定，即将进入 MVP 编码阶段

## 一、定位

**JPOV 是什么：** 一个面向 AI 自测输出的、支持流式绘制 + 简单交互的轻量可视化沙盒。

**JPOV 不是什么：** 游戏引擎、多模块调度框架、后端集成框架、异步资源管理系统。

### 核心理念

JPOV 是一个**纯函数式渲染步进**：给定输入，产出一帧画面。它不自带后端、不管理线程、不处理异步。

```
后端                                JPOV
  │                                   │
  │  (用户自行管理的 double buffer)    │
  ├──── ExtraData ───────────────────► │
  │                                   │
  │              InputSnapshot         │
  │  (键盘/鼠标) ──────────────────►   │
  │                                   │
  │              Camera               │
  │  (外部设置) ────────────────────►  │
  │                                   │
  │  ◄──── RenderCommandList ─────────┤
  │                                   │
```

后端和 JPOV 完全解耦。后端怎么跑、跑多快，JPOV 不知道也不关心。用户自己负责线程安全（double buffer + 加锁）。

### 为何不是多模块架构

早期设计曾引入多模块（模块声明 period、数据槽 Slot、暂存区 commit 等机制），试图同时覆盖：
- AI 画面交互（高频）
- 后端集成测试（低频 + 高频组合）

但实际推演发现：
1. MVP 阶段根本用不到多模块 —— 只有一个渲染模块
2. 多模块的 Slot/commit 机制在单模块下是死代码
3. "低频 + 高频集成测试"的愿景不现实 —— 后端输出空间太大，AI 单帧演进无法验证

**结论：一刀切掉所有复杂度。JPOV 只做一件事，做好它。**

## 二、MVP 范围

### Two Hard Things 原则

> "There are only two hard things in software development: naming things and cache invalidation."
>
> JPOV 的 MVP 阶段不做任何缓存。每帧重建一切。

### 什么是流式绘制

流式绘制 = 每帧从零开始构建绘制指令，不假设任何常驻资源。

```
每一帧：
  1. OneIteration 收到 InputSnapshot + ExtraData + Camera
  2. 从零开始构造绘制指令（坐标、颜色、文本）
  3. 输出 RenderCommandList
  4. 下一帧重新开始
```

没有纹理缓存、没有 mesh 池、没有材质加载。所有数据都是字面量。

### 支持的绘制类型

**2D（屏幕空间，坐标与窗口像素对齐）**

| 函数 | 说明 |
|------|------|
| `DrawLine(p1, p2, color, width)` | 线段 |
| `DrawRect(pos, size, color, filled)` | 矩形（实心/空心） |
| `DrawCircle(center, radius, color, filled)` | 圆形（实心/空心） |
| `DrawText(str, pos, font_size, color)` | 文本 |

**3D（世界空间，右手系）**

| 函数 | 说明 |
|------|------|
| `DrawLine3D(p1, p2, color, width)` | 线段 |
| `DrawTriangle3D(p1, p2, p3, color, filled)` | 三角形（实心/空心） |
| `DrawWireBox(min, max, color)` | 包围盒线框 |
| `DrawText3D(str, pos, font_size, color)` | 面向摄像机的标签 |

### 交互支持

- 输入：鼠标位置、鼠标按键、键盘按键（状态快照，非事件序列）
- 反馈：在 OneIteration 内处理简单的 UI 交互（hover、点击、拖拽、下拉）
- 限制：复杂的 UI 逻辑（响应式布局、复杂状态机）不属于 MVP

### 渲染策略

```
渲染顺序：先 3D → 后 2D（画家算法）
- 3D 内部：深度测试自动处理遮挡
- 2D 内部：按绘制顺序，后画的在上层（无深度测试）
- Alpha blend：2D 和 3D 都默认开启
```

## 三、核心 API

### OneIteration（JPOV 的唯一核心抽象）

```cpp
class OneIteration {
public:
    virtual RenderCommandList Step(
        const InputSnapshot& input,
        const ExtraData& user_data,
        const Camera& camera
    ) = 0;
};
```

| 参数 | 类型 | 来源 | 说明 |
|------|------|------|------|
| `input` | `InputSnapshot` | JPOV 内部（窗口系统） | 当前帧的键鼠状态 |
| `user_data` | `ExtraData` | 用户注入 | 后端产出的任意数据（double buffer） |
| `camera` | `Camera` | 用户设置 | 3D 投影参数 |

### 数据定义

```cpp
// === 输入 ===
struct InputSnapshot {
    // 鼠标
    Vec2  mouse_position;        // 窗口坐标
    bool  mouse_left_down;
    bool  mouse_right_down;
    bool  mouse_middle_down;
    
    // 键盘（按需检定的 key）
    bool IsKeyDown(KeyCode key) const;
    bool IsKeyPressed(KeyCode key) const;  // 本帧新按下
    bool IsKeyReleased(KeyCode key) const;
    
    // 修饰键
    bool ctrl_down;
    bool shift_down;
    bool alt_down;
};

// === 相机 ===
struct Camera {
    Vec3  position;     // 相机位置
    Vec3  target;       // 看向的点
    Vec3  up;           // 上方向，默认 {0, 1, 0}
    float fov;          // 视野（度），默认 60
    float near;         // 近裁剪面，默认 0.1
    float far;          // 远裁剪面，默认 1000.0
};

// === 渲染指令 ===
struct RenderCommandList {
    std::vector<DrawCommand> commands;
    // 可选：AI 校验的结构化信息
    std::vector<std::string> debug_labels;  // 每帧可选的语义标签
};

enum class DrawCommandType {
    Stream2D,
    Stream3D,
};

struct DrawCommand {
    DrawCommandType type;
    std::vector<Vertex> vertices;     // 顶点列表
    std::vector<uint32_t> indices;    // 索引（可选）
    std::string debug_label;          // 语义标签，供 AI 校验
};
```

### ExtraData（用户自管理的泛型数据）

```cpp
// JPOV 不定义 ExtraData 的具体结构
// 用户根据自己的后端场景定义
// 例如：
struct MyGameState {
    Vec3 player_position;
    std::vector<Enemy> enemies;
    float health;
    // ...
};
```

JPOV 通过 `std::any` 或模板接收 `ExtraData`，不检查、不解析、不序列化。

### 坐标约定

- **2D**：屏幕像素坐标，原点窗口左上角（x→右，y→下）
- **3D**：世界空间，右手系（x→右，y→上，z→后）
- **2D 文本**：基线对齐，位置为文本左下角

## 四、用户工作流

### 标准用法

```cpp
// 1. 继承 OneIteration
class MyScene : public jpov::OneIteration {
    RenderCommandList Step(
        const InputSnapshot& input,
        const ExtraData& user_data,
        const Camera& camera
    ) override {
        RenderCommandList cmds;
        
        // 从 ExtraData 获取后端状态（用户自己转换）
        auto& state = std::any_cast<const MyGameState&>(user_data);
        
        // 绘制
        cmds.DrawCircle(input.mouse_position, 10, Color::Red);
        cmds.DrawText3D("Hello JPOV", state.player_position, 16, Color::White);
        
        return cmds;
    }
};

// 2. 创建窗口并运行
jpov::Window window("JPOV Demo", 1280, 720);
MyScene scene;
window.SetCamera(Camera{...});
window.Run(&scene);  // 30fps 循环
```

### AI 自测

```cpp
// AI 构造的测试样例
MyGameState fake_state = { .player_position = {0, 0, 0}, .health = 100 };
Camera camera = { .position = {0, 0, 10}, .target = {0, 0, 0} };

// 仿真（不依赖窗口、不依赖实时时钟）
auto cmds = scene.Step(
    InputSnapshot{ .mouse_position = {100, 200} },
    std::make_any<MyGameState>(fake_state),
    camera
);

// AI 校验渲染指令
ASSERT(cmds.HasLabel("player_health_bar"));
ASSERT(cmds.GetLabel("player_health_bar").vertices.size() > 0);
```

## 五、与后端解耦的约定

JPOV 对后端不做任何假设，但以下是推荐的协作模式：

### 推荐模式：Double Buffer

```
后端线程（用户托管）
  while (running) {
    compute(dst_buffer);           // 计算结果
    swap(dst_buffer, src_buffer);  // 双缓冲切换
  }

主线程（JPOV 30fps）
  while (running) {
    // 从 src_buffer 读取最新结果
    ExtraData data = std::make_any<LatestState>(src_buffer->Read());
    auto cmds = scene.Step(input, data, camera);
    render(cmds);
    // 每 33ms 重复
  }
```

### 超时策略（JPOV 不处理，用户自行决定）

| 策略 | 适用场景 | 说明 |
|------|----------|------|
| 等 | 无实时性要求 | OneIteration 跑完为止 |
| 跳过（最新可用） | 实时交互应用 | 超过 33ms 直接取 InputSnapshot 和 ExtraData 的最新值重跑 |
| 报警 | 调试/开发 | 记录超时日志 |

JPOV 默认采用"等"策略（确定性优先），用户在窗口层可配置超时回调。

## 六、扩展性预留

MVP 只做最简接口，但接口设计预留了扩展方向：

### 绘制类型扩展

```cpp
enum class DrawCommandType {
    Stream2D,       // MVP
    Stream3D,       // MVP
    // 未来
    StaticMesh,     // 常驻 mesh：mesh_id + transform
    ImGuiBatch,     // ImGui 流式几何
    ParticleSystem, // 粒子系统
};
```

### 多 API 风格

MVP 只做 Immediate Mode OneIteration。未来可以添加：

- **保留模式**：常驻场景图，增量更新
- **声明式**：数据驱动场景描述

但这些都不在 MVP 范围。

## 七、非功能性约束

| 约束 | 目标 | 说明 |
|------|------|------|
| 语言 | C++20 | 与 ai_infra 项目一致 |
| 构建 | Bazel 5.4 + WORKSPACE | 传统模式，不依赖 Bzlmod |
| 外部依赖 | 最小化 | MVP 不引入渲染库，先做软件渲染器 |
| 平台 | Linux / Windows（MinGW） | 跨平台编译支持 |
| 单帧耗时 | < 33ms | 30fps 软实时目标 |

## 八、已排除（Future Work）

以下曾有讨论，最终排除在 MVP 和可预见的未来之外：

- **多模块调度**（thread + period + data slot + commit）
- **低频 + 高频集成测试**（单帧演进无法验证复杂时序交互）
- **网络/异步 I/O**（资源加载、远程后端）
- **复杂 UI 系统**（响应式布局、动画曲线、复杂状态机）
- **渲染后端抽象层**（MVP 直接实现一个简单后端，不抽象）
- **ImGui 集成**（不进接口，进具体实现——如果有的话）

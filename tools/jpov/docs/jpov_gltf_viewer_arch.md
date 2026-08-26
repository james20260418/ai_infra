# JPOV glTF 交互查看器 — 架构文档（task #2）

> 目的：在写任何实现代码前，把"交互查看 + `--four_views` 拍照"两条路径的
> 复用方式想清楚。核心结论：**只有一个渲染入口 `OneIteration`，交互与拍照只是
> 对同一个 `ViewConfig` 的不同驱动方式**（交互由输入实时改，拍照由固定序列驱动）。
> 这样 zero 代码分叉，交互截屏与离线 gold 天然一致（JPOV 框架本身保证
> `Run()` 与 `RunOnce()` 共享 `RunOnceInternal`，见 `jpov.h`）。

---

## 1. 需求回顾（来自 task #1 leader 留言）

| 能力 | 要求 |
|------|------|
| 形态 | JPOV 附带的小工具，`sh` 脚本包裹编译，默认 1280×720 窗口、渲染分辨率同、不可 resize |
| 坐标 | y-up，相机默认 `(1,1,1)→(0,1,0)` |
| 场景 | 300×300 高粗糙灰色地平面 quad + 被加载的 glTF 模型 |
| 光照 | `DaySkyCommand` 正午配置（同 sun_path 测试：由 sky 推导平行光 + 全局 ambient），太阳方向 `0,-1,-1` |
| glTF 路径 | 产物 ELF 第一参数给定（相对/绝对路径均可） |
| 帧率 | 60 帧 |
| 交互 | 右键 drag 转视角；滚轮 zoom（R 缩放） |
| 相机规律 | 始终看向 `(0,1,0)`；`position = (R cosφ sinθ, 1+R sinφ, R cosφ cosθ)`；水平右拖 1280px = θ 转 360°；垂直 720px = φ 变 180°；φ clamp ±90°，θ 不限 |
| zoom | 每滚一格 R ×√2 或 ×1/√2；R ∈ [0.1, 300] |
| AI 自查 | ELF 声明 `--four_views` → headless，渲染 4 个角度 (φ,θ)：`(0,0)`/`(90,0)`/`(0,90)`/`(45,45)`，输出 `front/up/left/perspective` 后缀图到同级目录，不弹窗 |

---

## 2. 核心抽象：`ViewConfig`

一个"视角配置"就是一条独占地决定相机姿态的元数据。交互与拍照都只需要
**持有并消费一个 ViewConfig**，差别仅在"谁在改它"。

```cpp
// 用户侧视角配置：用球面角描述相机绕目标点 (0,1,0) 的环绕。
//   phi   — 仰角（弧度）。相机在目标点上方的俯仰量，clamp 到 [-π/2, +π/2]。
//           注意 phi 用弧度存储，但交互映射 / 拍照固定角度用"度"表达更直观，
//           边界处做单位换算（见 §5）。内部统一 radians。
//   theta — 水平方位角（弧度）。绕 y 轴，无 clamp（可无限转）。
//   R     — 相机到目标点的距离（米）。范围 [0.1, 300]。
// 相机位置由公式推导（y-up）：
//   position.x = R * cos(phi) * sin(theta)
//   position.y = 1 + R * sin(phi)
//   position.z = R * cos(phi) * cos(theta)
//   target     = {0, 1, 0}
struct ViewConfig {
    // 统一用弧度存储，避免"这个接口收度还是收弧度"的歧义（量纲约定见 §5）。
    double phi   = 0.0;   // 仰角（弧度）
    double theta = 0.0;   // 方位角（弧度）
    double R     = 10.0;  // 距离（米）

    // 把当前 ViewConfig 写入相机。up 恒为 y-up。
    // Pre-condition: R 在 [0.1, 300]，phi 已 clamp 到 [-π/2, π/2]
    jpov::Vec3f Position() const;
    jpov::Vec3f Target() const { return {0.0f, 1.0f, 0.0f}; }  // 恒定
};

// 交互输入 → 更新 ViewConfig（仅 interactive 模式调用）。
// dx/dy：本帧右键 drag 位移（像素）；scroll：本帧滚轮增量（格，单位 +1/-1）。
// window_w / window_h：窗口尺寸（1280×720），用于把像素位移映射成角度。
// Pre-condition: window_w, window_h > 0
void ApplyInput(ViewConfig* v /*inout*/, float dx, float dy, float scroll,
                int window_w, int window_h);
```

**为什么这样设计**：

- **单一数据源**：相机姿态完全由 `ViewConfig` 决定，消除"交互维护自己的角度状态、
  拍照又写死另一套相机"的分叉。
- **零分支渲染**：`OneIteration` 只读当前 `ViewConfig`，不关心它是被交互改的还是
  拍照序列赋的。
- **可测试**：`Position()`/`ApplyInput()` 是纯函数，可独立单测（phi clamp、R clamp、
  像素→角度比例）。

---

## 3. 渲染入口复用（核心）

JPOV 框架已保证 `Run()`（交互多帧）与 `RunOnce()`（单帧+截图）共享
`OneIteration`。因此两条路径共用**同一个渲染函数**：

```
OneIteration(frame_count, input, winfo, cmds)
  ├─ camera.fbo_3d_width_/height_ = 1280 / 720（渲染分辨率与窗口一致）
  ├─ 从 ViewConfig 推导并写入 cmds->camera（position/target/up/near/far/fov）
  ├─ cmds->sky   = 正午 DaySkyCommand（turbidity=2, season 中性, intensity=1.0,
  │                sun_dir = -(0,-1,-1) = {0,1,1}，sun_radius/brightness 见 §6）
  ├─ cmds->sun   = DirectionalLight{ direction={0,-1,-1}, color=sky.DirectionalColor(),
  │                intensity=sky.DirectionalIntensity() }   // 由 sky 推导
  ├─ cmds->ambient = AmbientLight{ color=sky.AmbientColor(), intensity=0.3 }
  │                // "0.3" 是 LIGHT_INTENSITY.md 的正午阴影基准，非 0.5（见 §5 教训）
  ├─ cmds->tone_mapping = true
  ├─ DrawObject3D(ground_mesh, ground_mat, ...)   // 300×300 灰高粗糙 quad
  ├─ DrawGltfObject(gltf_, {0,0,0}, up={0,1,0}, front={0,0,1})
  └─ （interactive 模式额外）入口参数校验 gltf_ 非空
```

**两个运行模式 = 同一个 app 类的两种驱动**：

```cpp
class GltfViewerApp : public JPOV {
public:
    // 资源：Init() 里 LoadGltf + RegisterMesh(地面)
    jpov::GltfObject gltf_;     // 待查看模型
    uint32_t ground_mesh_ = 0;  // 300×300 地面 quad
    jpov::PBRMaterial ground_mat_;  // 高粗糙灰色
    ViewConfig view_;           // 当前视角（interactive 由输入改；four_views 由序列赋）

    void OneIteration(...) override {
        // §3 的共享渲染体，只读 view_
    }
};
```

- **交互模式**：`main()` 里跑 `app.Run()`。`OneIteration` 开头用 `ApplyInput(&view_,
  input.mouse_dx, input.mouse_dy, input.scroll_delta, winfo.width, winfo.height)` 更新
  `view_`，再渲染同一份场景。60fps 由 `Config.target_fps=60` 保证。
- **`--four_views` 模式**：`main()` 检测到第一个参数是 `--four_views` →
  设 `Config.headless=true`，不建可见窗口；对 4 组 (phi,theta)：
  `view_.phi/theta` 赋固定弧度值 + `view_.R` 用同一默认值，然后 `RunOnce(input, winfo, out_png)`。
  输出到 glTF 所在目录，文件名 `{gltf_basename}_front/up/left/perspective.png`。

---

## 4. 复用要点（评审关注清单）

1. **渲染体零分支**：`OneIteration` 内**不出现 `if (interactive) {...} else {...}`**。
   交互 vs 拍照的唯一差异在 main 的 `Run()` vs `RunOnce()`，以及 `ViewConfig` 的来源。
   - 若必须区分（如 four_views 想关掉 sun 盘或隐藏某种细节），使用 app 上一个
     `bool headless_shot_` 标志，且默认 false 以对齐交互观感（gold 自证保证不分叉）。
2. **视角配置结构体**（§2）复用 `Position()`：交互与拍照调用同一推导，杜绝
   "交互一套公式、拍照另一套公式"的经典分叉（对标 mrquad generator/test 分叉教训）。
   - **唯一 headless 区分**（arch §4 note#1 认可的机制）：`--four_views` 拍照时相机
     **目标点改为模型原点 (0,0,0)**（`GltfViewerApp::headless_shot_=true`）。原因：交互默认
     看向 (0,1,0)，但小模型贴地画在原点，phi=0 的 front/left 会从 y=1 平视、从模型头顶
     看过去，模型落在视锥下缘之外拍不到（task#7 自测实测 bug）。拍照改成看模型原点后，
     4 个角度都能框住模型。交互模式不受影响（保持看向 (0,1,0)）。
3. **光照配置提成"正午基准"单例函数**：`MakeNoonLighting()` 返回
   `{sky, sun, ambient}` 三元组，interactive/four_views/未来 gold 测试三处共用。
   （对标 sun_path 的 `SkyDirectionAmbient` 结构，那个已经做对了，直接借鉴。）
4. **地面材质**：`ground_mat_` 一次性构造，`RegisterMesh` 一次；不要在 OneIteration
   里重复构造/上传。
5. **glTF 路径解析**：`main` 开头用 `argv[1]`（相对/绝对）在 `Init()` 前拿到，
   `LoadGltf` 失败（`empty()`）→ `LOG(FATAL)` 带清晰信息，不静默。

---

## 5. 量纲与关键常数（写死在文档，实现照抄）

| 项 | 值 | 说明 |
|----|----|------|
| 渲染/窗口分辨率 | 1280 × 720 | `Config.width/height` + `fbo_3d_width_/height_` 统一 |
| 窗口 resize | 不可 resize | `Config.resizable = false` |
| fov | 60° | 默认 `Camera::fov` |
| near / far | 0.05 / 1000 | 沿用 sun_path/standard_sunny_day 的 near=0.05；场景 R 最大 300，far 足够 |
| R 范围 | [0.1, 300] 米 | 滚轮 clamp，`std::clamp` |
| R 滚轮倍率 | ×√2（上滚）/ ×1/√2（下滚） | 每格一次乘/除 |
| 像素→角度 | 水平 1280px = 360°，垂直 720px = 180° | `θ += dx * (2π/1280)`；`φ += dy * (π/720)` |
| φ clamp | ±90°（±π/2 弧度） | `std::clamp`，θ 不 clamp |
| 太阳传播方向 | `{0,-1,-1}` | `sun.direction`；`sky.sun_dir = {0,1,1}` |
| 正午 sky | turbidity=2, season 中性, intensity=1.0 | 同 sun_path `DefaultSunPath()[0]` |
| 正午 ambient | intensity = 0.3 | **必须 0.3**（LIGHT_INTENSITY.md 正午阴影基准），不是 0.5 |
| 地面材质 | 高粗糙（roughness ≈ 1）、灰色 | 300×300 单位：米（quad 半宽 150） |
| 相机目标 | `(0,1,0)`（交互）；拍照为 `(0,0,0)` 模型原点 | 交互恒定 (0,1,0)；`--four_views` 因要拍到小模型，目标点改到模型原点（见 §4 note#2） |
| 相机位置 | `(R cosφ sinθ, 1+R sinφ, R cosφ cosθ)` | y-up |
| 默认初始视角 | `(1,1,1)→(0,1,0)` 即 R=√2≈1.414, θ=45°(π/4), φ≈arcsin(1+0)/… 见下 | 见 §7 推导 |
| four_views 角度(度) | (0,0) / (90,0) / (0,90) / (45,45) | phi,theta；图片 front/up/left/perspective |

---

## 6. 正午光照的具体写法（照抄 sun_path `DefaultSunPath()[0]`）

```cpp
// 正午晴天：太阳光传播方向 {0,-1,-1}
const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
jpov::DaySkyCommand sky{
    /*sun_dir*/     jpov::Vec3f(-sun_light_dir.x(), -sun_light_dir.y(),
                                -sun_light_dir.z()),  // = {0,1,1}
    /*turbidity*/   2.0f,
    /*season*/      {1.0f, 1.0f, 1.0f, 1.0f},
    /*intensity*/   1.0f,
    /*ground_color*/{0.05f, 0.06f, 0.08f, 1.0f},
    /*sun_radius*/  0.02,
    /*sun_brightness*/ 1e3,
    /*sun_glow*/    1.0,
};
cmds->sky = sky;
cmds->sun = jpov::DirectionalLight{
    /*direction*/ sun_light_dir,
    /*color*/     sky.DirectionalColor(),      // 由 sky 推导
    /*intensity*/ sky.DirectionalIntensity(),  // 由 sky 推导（正午≈3.0）
};
cmds->ambient = jpov::AmbientLight{
    .color     = {1.0f, 1.0f, 1.0f, 1.0f},     // 或 sky.AmbientColor()
    .intensity = 0.3f,                         // 正午阴影基准（定稿）
};
cmds->tone_mapping = true;
```

> **关键经验（来自 PR #60/standard_sunny_day）**：ambient 强度必须用 0.3 的正午基准，
> 别手滑写成 0.5 —— PR #60 花大力气定标的"白天基准"就是 sun=3.0/ambient=0.3。
> 拍照模式与交互模式必须用同一组光照，否则 four_views 截图和交互观感不一致。

---

## 7. 默认初始视角推导（(1,1,1)→(0,1,0)）

起点相机 `position=(1,1,1)`，target=(0,1,0)：

- dx=1, dy=1-1=0, dz=1，初始水平距离 R_horizontal = √(dx²+dz²) = √2。
- 因 y 与 target 同高（dy=0），phi=0。
- θ 满足 sinθ = dx/Rₕ = 1/√2, cosθ = dz/Rₕ = 1/√2 → θ = π/4。
- R = 总距离 = √(Rₕ² + dy²) = √2。

即 `ViewConfig{phi=0, theta=π/4, R=√2}` 精确复现默认视角。实现就写这个常量。

---

## 8. 文件组织与 BUILD（实现阶段落地）

```
tools/jpov/
  demo/jpov_gltf_viewer.cc        # 主程序: main + GltfViewerApp
  demo/view_config.h              # §2 的 ViewConfig + ApplyInput（header-only，可单测）
  demo/view_config_test.cc        # 纯函数单测（Position/ApplyInput/clamp）
  build_jpov_gltf_viewer.sh       # sh 包裹编译（仿 build_jpov_ui_demo.sh）
  docs/jpov_gltf_viewer_arch.md   # 本文档
```

BUILD 加 `cc_binary`（仿 `jpov_ui_demo`）：deps = `include/jpov:jpov` + glog;
Windows `_exe` 变体可延后（leader 只说 sh 脚本，双平台非硬性，先 Linux）。

---

## 9. 难度评估与分步建议（leader 要求） 

| 子步骤 | 内容 | 难度 |
|--------|------|------|
| 已完成 | 前置：拉 main、建 feature 分支 | 低 |
| task#2 | 本文档（架构） | 低~中 |
| 后续 task | 实现 `ViewConfig` + 渲染体 + 交互（右键/滚轮） | 中 |
| 后续 task | `--four_views` headless 拍照 + sh 脚本 + 自测 | 中 |
| 后续 task | 用仓里现有 gltf（如 `test/object3d/scene_assets/` 的 pliers/stool/table）验收 + 功能自测说明 | 中 |

**"高效复用"的一句话总结**：把所有能共享的都收进 `GltfViewerApp` 的成员与
`ViewConfig`，让交互和拍照走同一条 `OneIteration` + 同一份 `MakeNoonLighting()`，
就能保证 zero 分叉、截图即所见。

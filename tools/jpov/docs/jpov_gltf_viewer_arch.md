# JPOV glTF 交互查看器 — 架构笔记

> 本文件是 task #1 的架构产物。任务背景（leader #8 批示）：复用仓库已有
> `feature/20260824-gltf-viewer` 分支的实现，但**不得照抄，用前仔细审视**。
> 本文档记录：① 我对复用分支的完整审视结论（哪些借、哪些改、为什么）；
> ② 本实现的设计骨架（ViewConfig 复用 + 两模式驱动同一渲染入口）。
> 实现落地时对照本笔记逐条核对，避免把分支的旧缺陷原样搬过来。

---

## 1. 审视基线（读 leader 需求逐条对照）

| 需求 | 要求 | 复用分支现状 | 审视结论 |
|------|------|--------------|----------|
| 形态 | JPOV 附带小工具，sh 脚本包裹编译 | `build_jpov_gltf_viewer.sh` 已实现 | ✅ 结构合理，复用 |
| 窗口 | 默认 1280×720，渲染同分辨率，不可 resize | `Config` + `fbo_3d_width_/height_` | ✅ 符合 |
| 坐标 | y-up，相机默认 (1,1,1)→(0,1,0) | `DefaultView()={phi=0,theta=π/4,R=√2}` | ✅ 推导正确（arch §7 已证） |
| 场景 | 300×300 高粗糙灰 ground quad + glTF | `MakeGroundQuad()` half=150 + `GroundMaterial()` | ✅ 符合；法线 +Y、y=0 |
| 光照 | DaySkyCommand 正午，太阳方向 (0,-1,-1)，sky 推导平行光+ambient | `MakeNoonLighting()` | ✅ 复用 sun_path 模式，ambient=0.3 |
| glTF 路径 | 产物第一参数给定（相对/绝对） | `FirstNonFlagArg()` | ✅ 符合 |
| 帧率 | 60 fps | `cfg.target_fps=60` | ✅ 符合 |
| 交互 | 右键 drag 视角 + 滚轮 zoom（R） | `ApplyInput()` | ✅ 公式与需求一致（见 §4） |
| AI 自查 | `--four_views`：headless 渲染 4 角度，输出 front/up/left/perspective 到同级目录，不弹窗 | `RunFourViews()` | ✅ 符合（见 §5） |

**总体判断**：复用分支的 7 个文件（1 架构文档 / 1 header / 2 demo / 1 测试 / 1 BUILD / 1 sh）
质量高、与需求逐条对齐，值得作为实现基点。但它**从未合入 main、落后 main 5 个提交、
领先 1 个提交**——直接 rebase 到最新 main 即可，冲突风险低（新增文件，BUILD 需小心并入）。

---

## 2. 复用时的审视要点（逐条核对，防止照抄旧盲点）

1. **交互与拍照零分叉是本设计核心**——`ViewConfig` 是唯一相机状态，`OneIteration`
   只读它，交互/拍照只是"谁在改它"不同。此点完全正确，**必须保留**。
2. **光照量纲对齐（PR #60 教训）**：`MakeNoonLighting` 的 ambient 必须 0.3
   （LIGHT_INTENSITY.md 正午阴影基准），不能手滑 0.5。分支已做对，保留。
3. **唯一 headless 区分点**：`--four_views` 拍照时相机**目标点改到模型原点 (0,0,0)**。
   这是分支实测后加的必要修正（交互默认看 (0,1,0)，phi=0 时小模型落在视锥下缘外
   拍不到）。**该区分是合理的**，保留，但要保证交互模式仍默认看 (0,1,0) 不受影响。
4. **BUILD 作用域**：分支把产物/测试限制在 `linux_x86_64`。需求只要求 sh 脚本 Linux
   路径，此限制合理，保留；但合并到最新 main 时要与 main 现有 BUILD 结构核对，
   不覆盖 main 里已新增的内容（PR #68 字体相关）。
5. **滚轮符号约定需仔细核对**：实现里 `scroll>0` 是缩小（R ×1/√2）、`scroll<0` 是放大
   （R ×√2）。需求表述"每滚一格 R 变 √2 或 0.5√2"方向语义依赖 GLFW 的 scroll 正负约定。
   **落地时以单测为准**（view_config_test 已覆盖 ±），并确认与交互手感一致。
6. **`jpov_gltf_viewer_smoke.cc`** 是开发自检用的 headless 冒烟渲染，非 leader 需求产物，
   但利于验证，保留作自检工具（不进交付说明也行，视需要）。

---

## 3. 设计骨架：单一渲染入口 + ViewConfig

```
GltfViewerApp : JPOV
  ├─ ViewConfig view_          // 唯一相机状态（phi/theta/R）
  ├─ NoonLighting noon_        // sky + sun(方向光) + ambient（一次构造）
  ├─ uint32 ground_mesh_       // 300×300 ground quad（一次 RegisterMesh）
  ├─ PBRMaterial ground_mat_   // 高粗糙灰色（一次构造）
  ├─ GltfObject gltf_          // 被查看模型（Init 后 LoadGltf）
  ├─ bool headless_shot_       // true=拍照模式（看模型原点）
  └─ OneIteration(frame, input, winfo, cmds) override
       ├─ cmds->camera = 由 view_.Position() + Target() 推导
       ├─ cmds->sky/sun/ambient/tone_mapping = noon_（正午）
       ├─ DrawObject3D(ground) + DrawGltfObject(gltf)   // 零分支
       └─ （交互模式）先 ApplyInput(&view_, dx,dy,scroll) 再渲染

驱动方式：
  交互模式  → main: app.Run()          // 窗口事件循环，OneIteration 每帧被 input 驱动
  --four_views → main: RunFourViews()  // headless，逐个赋固定 view_ 后 RunOnce 截图
```

**复用与零分叉保证**：交互与拍照共享 `OneIteration` 同一个渲染体 + 同一份 `MakeNoonLighting()`
+ 同一个 `ViewConfig::Position()`。唯一的视角差异来自 `ViewConfig` 的值与 `headless_shot_`
目标点，不产生第二套渲染逻辑 → 截图即所见。

---

## 4. 相机参数化（ViewConfig 定义）

相机绕目标点 (0,1,0)（交互）/ 模型原点 (0,0,0)（拍照）环绕的球面坐标：

```
position.x = R * cos(phi) * sin(theta)
position.y = 1 + R * sin(phi)          // 交互向上抬 1；拍照目标改原点时 y=1+R sinφ
position.z = R * cos(phi) * cos(theta)
```

量纲约定：`phi/theta` 用**弧度**存储（避免"收度还是收弧"歧义）；`R` 单位米，范围
[0.1, 300]；`phi` clamp ±90°，`theta` 不 clamp。

交互映射（`ApplyInput`，窗口 1280×720）：
- 水平右拖 1280px = θ 转 360°（2π）→ `θ += dx * (2π/window_w)`
- 垂直右拖 720px = φ 变 180°（π）→ `φ += dy * (π/window_h)`（dy 向下为正、φ 向上为正，
  故取负）
- 滚轮每格 R ×√2（放大）或 ×/√2（缩小）；R clamp [0.1, 300]。
  注意：同帧多格 scroll 要按格数连乘倍率，而非当作单格。

默认初始视角：`(1,1,1)→(0,1,0)` 精确复现为 `{phi=0, theta=π/4, R=√2}`。

---

## 5. 正午光照（MakeNoonLighting）

太阳光传播方向 `{0,-1,-1}`；DaySkyCommand：turbidity=2、season 中性、intensity=1.0、
sun_dir=+`{0,1,1}`（取反）；平行光与 ambient 均**由 sky 推导**（task#3 验收）——
color 用 `DirectionalColor()/AmbientColor()`（色调随太阳仰角变），intensity 用
`DirectionalIntensity()/AmbientIntensity()`（相对衰减随太阳仰角变），但绝对强度锚定
LIGHT_INTENSITY.md 三·五 晴天正午基准：sun 基准 3.0、ambient 基准 0.3（PR #60 定标，
勿 0.5，勿裸 `AmbientIntensity()=1.0`，否则影子被 ACES 压没）；`tone_mapping=true`。
`near=0.05, far=1000, fov=60°`。

---

## 5a. 光照/场景调节滑条（交互版，2026-08-30 新增）

交互窗口底部居中新增 5 个半屏宽滑条（仅交互模式绘制；`--four_views` 拍照仍走固定
`MakeNoonLighting()`，截图不带面板，零回归）：

| # | 滑条 | 范围 | 默认 | 作用 |
|---|------|------|------|------|
| 1 | 太阳角度 θ | [0, π] 弧度 | 0 | 太阳光传播方向 = `{−sinθ, −cosθ, 0}`；θ=0 天顶直射（最亮），θ=π 从正下方 |
| 2 | 平行光强度 | [1, 10] | 3.0 | `DirectionalIntensity` 的 midday_intensity（同 LIGHT_INTENSITY.md 的 3.0） |
| 3 | 环境光强度 | [0.1, 1.0] | 0.3 | `AmbientIntensity` 的 noon_intensity（同当前的 0.3） |
| 4 | 地面高度 y | [-3, +3] 米 | -3 | 实时重建地面 quad（UpdateMesh），看物体落地面/阴影落地面 |
| 5 | 模型缩放 | [0.1, 20] | 1.0 | 整体缩放（先缩放再旋转平移，见 Object3DCommand::scale），验证小物体阴影 |

光照类滑条（1~3）：
- 仍全由 sky 推导——传给 sky 的 `sun_dir` = `−sun_light_dir = {sinθ, cosθ, 0}`，用其 y=cosθ
  驱动 `DirectionalColor()/AmbientColor()/DirectionalIntensity()/AmbientIntensity()`；只有基准强度
  （sun/ambient 两个滑条）由滑条给定，sky 其余参数（turbidity/season/intensity/ground_color/
  sun_radius/sun_brightness/sun_glow）不变。
- 交互与拍照共用 `OneIteration`，靠 `app.interactive_` 区分是否 `ui_.Emit`（同 arch §4 的 headless 区分思路）。

地面高度（4）：`MakeGroundQuad(float y=-3)` 参数化；交互时 `ground_y_` 变化则 `UpdateMesh`
（VBO 布局不变，原地更新）。
模型缩放（5）：`DrawObject3D/DrawGltfObject` 新增 `float scale=1.0`（签名最末，先缩放后旋转平移）；
  拾取/高亮 pass 同步使用（`Primitives3DRenderer::BuildModelMatrix` 同加 scale 参数）。

滑条用轻量级即时模式 UI（`//tools/jpov/interface:ui`），文本默认字体 CJK（滑条标签显中文）；
分发产物 exe 旁需 `fonts/`（DejaVu + NotoSansCJK，build 脚本同构拷贝，PR #68 优先 exe 旁路径）。

---

## 6. 文件组织

```
tools/jpov/
  demo/jpov_gltf_viewer.cc        # 主程序: main + GltfViewerApp + RunFourViews
  demo/view_config.h              # ViewConfig + ApplyInput + MakeNoonLighting + 地面（header-only）
  demo/view_config_test.cc        # 纯函数单测（Position/ApplyInput/clamp/光照）
  demo/jpov_gltf_viewer_smoke.cc  # headless 冒烟自检（可选）
  build_jpov_gltf_viewer.sh       # sh 包裹编译 → output/jpov_gltf_viewer/
  docs/jpov_gltf_viewer_arch.md   # 本文档
```

BUILD 新增：`view_config`(cc_library header) + `jpov_gltf_viewer`(cc_binary) +
`view_config_test`(cc_test) + `jpov_gltf_viewer_smoke`(cc_binary)。限制 linux_x86_64。

---

## 7. 落地核对清单（实现时逐条打勾）

- [ ] 从 main 创建 feature 分支，rebase 复用分支的 7 个文件，BUILD 小心并入（不覆盖 PR #68 内容）
- [ ] `ViewConfig` / `ApplyInput` / `MakeNoonLighting` / `MakeGroundQuad` 复用（审视后原样或微调）
- [ ] `.cc` 主程序走单渲染入口；`--four_views` 仅改 view_ + headless_shot_
- [ ] 目标点：交互看 (0,1,0)，拍照看 (0,0,0)（headless_shot_ 区分）
- [x] ~~ambient=0.3~~ 由 sky.AmbientIntensity(0.3) 推导（锚定 PR#60 基准）；near/far/fov 对齐 §5
- [ ] 单测通过（view_config_test）
- [ ] sh 脚本产出 output/jpov_gltf_viewer/ 可执行
- [ ] 用仓里现有 gltf（test/object3d/scene_assets 的 pliers/stool 等）实跑 four_views 出图验证

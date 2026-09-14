# JPOV 多 Pass 结构设计（渲染通行证 / Texture 槽位生命周期契约）

> 定稿日期：2026-09-12
> 适用范围：`tools/jpov/src/renderer.cc` 的 3D 渲染主流程 + 各子渲染器
> （`object3d/` `skeleton/` `primitives3d/` `skydome/` `font2d/` `primitives2d/`）。
> 配套阅读：`docs/jpov_renderer_isolation.md`（子渲染器隔离性静态审查）。

---

## 0. 本文回答什么问题

JPOV 的 3D 渲染不是「一次 draw 走到底」，而是**一串按顺序执行的 pass**。串 pass 会引入两类
跨 pass 的隐式依赖：

1. **资源依赖**：后面的 pass 要读前面 pass 产出的纹理（如阴影贴图、HDR 颜色）。
2. **状态依赖**：GL 的 texture unit / program / VAO / 状态位是**全局可变状态**，
   前一个 pass 留下的东西，后一个 pass 可能"意外继承"。

本文把这两类依赖**从注释里的口头约定，升级成可 review、可检查的契约**。

核心结论：

> **不该问「哪个子渲染器用几号 texture 槽」——该问「几号槽在哪个 pass 范围内有效」。**
> 槽位编号本身不是资源，**槽位的生命周期**才是。

---

## 1. JPOV 的 Pass 结构

一帧 3D 渲染的 pass 序列（`Renderer::Render`，按执行顺序）：

| # | Pass | 产出 | 目标缓冲 | 触发条件 |
|---|---|---|---|---|
| 0 | **Shadow pass**（CSM） | 5 级联光空间线性深度纹理 | 每级联独立 FBO | `cmds.sun` 有值 |
| 1 | **Sky pass** | 程序化天光（无纹理依赖） | HDR 3D FBO | `cmds.sky` 有值 |
| 2 | **Tile lighting 准备**（CPU） | tile 光源索引纹理 | 独立纹理 | `cmds.tile_culling` |
| 2.5 | **Picking pass** | color-ID 像素（CPU 读回） | 独立 pick FBO | `cmds.pick.enabled` |
| 3 | **Main 3D pass** | HDR 场景颜色（PBR/图元/蒙皮） | HDR 3D FBO | 有 3D 指令 |
| 4 | **Highlight pass** | 叠加恒定像素宽描边的颜色 | hl FBO（color-only） | `cmds.highlight_style` |
| 5 | **Bloom pass** | 加回辉光的 HDR | bloom 链 FBO | `cmds.bloom->enabled` |
| 6 | **Tone map pass** | LDR 颜色（ACES + sRGB + 微调） | 主 FBO | `cmds.tone_mapping` |

**关键结构事实**：

* Pass 0~2.5 是**准备阶段**（produce），pass 3 是**主要消费阶段**（main draw）。
* Pass 4~6 是**后处理阶段**（post），输入是 pass 3 的颜色纹理，**不再回到 3D 几何绘制**。
* **所有子渲染器的 draw 都发生在 pass 3 内**（`Draw3DCommands` 按 `cmds.order` 遍历各命令池）。
* Pass 7（2D）在 `Renderer::Render` 的主 FBO 阶段，与 3D 段之间用 `glPushAttrib/glPopAttrib` 隔开。

> ⚠️ Pass 4/5（Highlight / Bloom）在实现上是**与主 3D 段并列的后处理子步骤**，
> 而 shadow / tone map 也是并列的。它们互相之间通过「颜色纹理」单向链接，
> **不共享 texture 槽位**（各自在自己内部用 0/1 槽，用完 `glActiveTexture(GL_TEXTURE0)`）。

---

## 2. DAG：谁读谁的产出

```
                   ┌──────────────────────┐
                   │  pass 0: Shadow (CSM)│──┐
                   └──────────────────────┘  │ 级联深度纹理 ×5
                                             │ (共享只读)
                   ┌──────────────────────┐  │
                   │  pass 1: Sky         │  │
                   └──────────┬───────────┘  │
                              │ 写入         │
                              ▼              ▼
                   ┌──────────────────────────────────┐
                   │  pass 3: Main 3D                 │
                   │   object3d │ skeleton │ prims3d  │
                   │   （消费:级联 + tile 索引 + 材质）│
                   └──────────┬───────────────────────┘
                              │ HDR 颜色
                              ▼
                   ┌──────────────────────┐   ┌────────────────┐
                   │ pass 4: Highlight    │◄──│ pass 2.5: Pick │
                   └──────────┬───────────┘   └────────────────┘
                              ▼
                   ┌──────────────────────┐
                   │ pass 5: Bloom        │
                   └──────────┬───────────┘
                              ▼
                   ┌──────────────────────┐
                   │ pass 6: Tone map     │
                   └──────────┬───────────┘
                              ▼
                   ┌──────────────────────┐
                   │ pass 7: 2D           │
                   └──────────────────────┘
```

**由此得出两类槽位**：`pass 0→3` 的级联纹理是**跨 pass 共享只读**；
`pass 3` 内部的材质纹理是**子渲染器各自私有**。这个区分是全文的基础。

---

## 3. 两类 Texture 槽位

### 3.1 独占槽（Exclusive Slot）

**定义**：槽里绑的纹理，只有**一个子渲染器**会读，且它的生命周期**不跨子渲染器**。

**契约**：

* 谁用谁绑，**绑完即用，用完归零 active unit**。
* **不进任何中央分配表** —— 因为不存在跨子渲染器协商的需要。
* 唯一约束：**同一 pass 内，不得有两个语义指向同一槽**（见 §5 规则 R1）。

**现有独占槽**：

| 槽位 | 使用者 | 内容 | 绑定位置 |
|---|---|---|---|
| 1..6 | `Object3DRenderer` | 材质：baseColor / metallic / roughness / emissive / AO / normal | `DrawObject3D` 内逐通道 |
| 1..6 | `SkeletonRenderer`（主 pass） | 同上 6 通道 | `DrawSkinnedMesh` 内逐通道 |
| 12 | `SkeletonRenderer`（主 pass） | pose atlas | `DrawSkinnedMesh`（`skeleton_renderer.cc:222`） |
| 7 | `SkeletonRenderer`（**阴影 pass**） | pose atlas | `DrawSkinnedMeshShadow`（`:341`） |
| 0 | `FontRenderer`（**3D 文本**） | 字形 atlas | `FontRenderer::DrawText3D`（主 pass 内） |
| 0 | 各 2D 子渲染器 | image / text atlas | draw 内 |

> 💡 **3D 文本（`kText3D`）完全落在主 pass 内，且只占槽 0**（字形 atlas）。
> 它复用 2D 的字体 atlas / 排版 / 对齐，所以不引入新槽位语义；
> 对 §5 规则 R1 无影响。但它有一条**额外的 GL 状态约束**：必须在 draw 前
> `glDisable(GL_CULL_FACE)` —— 字形三角形在「纹理平面 y 向下」约定下映射到
> 世界后从正面看是**顺时针**，会被主 pass 的 `glCullFace(GL_BACK)` 整片剔除。
> 详见 `primitives3d_renderer.h` 的 `kText3dFs` 与 `font_renderer.cc` 的
> `DrawText3D` 注释。

> 💡 **注意 7 与 12 的"双重语义"是合法的**：7 在 obj3d 语境是 cascade-0，在
> skeleton 阴影 pass 语境是 pose atlas。二者**不同时活跃**（§3.2 会解释为什么这仍要小心）。

### 3.2 共享槽（Shared Slot）

**定义**：槽里绑的纹理由**多个子渲染器**在**同一个 pass 内**读取。

**契约**：

* 在**该 pass 的入口**绑一次（而不是每 draw 重绑），声明给所有需要它的 program。
* **全 pass 内保持绑定**，任何参与者不得覆盖。
* **必须进中央槽位表**（§4）—— 因为这是跨子渲染器协商的唯一场景。

**现有共享槽**：

| 槽位 | 内容 | 绑定者 | 消费者 |
|---|---|---|---|
| 7..11 | CSM 级联 0..4 阴影深度 | `Renderer::UploadSunData` | obj3d PBR（2 版 program）+ skeleton PBR |
| 0 | tile 光源索引纹理 | `DrawObject3D` 每次 draw | obj3d PBR |

> `tile_index_tex` 目前被 obj3d 在两个 program 间共享、**每 draw 重绑**。成本≈0（§6），
> 但它严格说是"独占槽的共享使用"，因为它只有 obj3d 一个消费者。

---

## 4. CSM 级联的槽位约定：**高编号段**

### 4.1 为什么级联该占高编号

**级联层数是运行期可变的**（`ShadowConfig::cascade_count ∈ [1, kMaxCascades=5]`）。
shader 侧数组长度定死为 `kMaxCascades`：

```glsl
uniform sampler2D uShadowMap[5];   // 长度 = kMaxCascades，与运行期 cascade_count 无关
```

**因此级联的槽位占用是"上界式"的**：无论本次实际用 1 级还是 5 级，
**槽位段 [cascade_base, cascade_base + kMaxCascades) 都必须整体保留**，
因为：

1. shader 的 `uniform sampler2D uShadowMap[5]` 需要 5 个连续槽位才能全部有效声明
   （GL 规范：数组 sampler 占**连续**槽位）。
2. 运行期改 `cascade_count` 不应要求重新规划槽位表。

**结论**：级联段应放在**编号高的一段**，让它上方留出连续的"可增长空间"给
未来可能变多的级联，同时**不挡住**低编号的常用独占槽。

### 4.2 当前约定

```
cascade_base = 7
级联 c 占用槽 = 7 + c          (c ∈ [0, kMaxCascades))
保留段 = [7, 7 + kMaxCascades) = [7, 12)   ← 即使只用 1 级，7..11 整段保留
```

**改动约束**：`kMaxCascades` 一旦上调（如 5 → 8），级联段会从 `[7,12)` 扩张到 `[7,15)`，
**必须同步重排其上的槽位**。当前 12 被 skeleton 主 pass 的 pose atlas 占用，
所以 **kMaxCascades 的上调上限是「不撞上 12」** —— 即最多到 5（现状）。

> ⚠️ 这是当前槽位表里**最脆弱的一处**：12 这个"手挑的空号"正好卡在级联段的扩张方向上。
> 若将来要支持 >5 级联，必须先把 skeleton 的独占槽挪出 12。

### 4.3 建议（未实施）

若要给级联留更大的扩张空间，把共享段整体上移：

```
0         tile 光源索引（独占）
1..6      材质 6 通道（独占）
7..11     ← 现 CSM；建议改为 16..20（或更高的对齐段）
12..15    预留给级联扩张 / 新的共享资源
16+       独占槽的"新家"（skeleton pose atlas 等）
```

**代价**：改槽位要动 shader 源码里的硬编码 `glUniform1i(uShadowMap[c], 7+c)`，
且**必须 gold 图验证**。收益只是"扩张空间更大"。
**结论：现状可用，暂不动；若要动，单独一轮。**

---

## 5. 硬规则（可进 CI）

### R1 — 同一 pass 内不得有两个语义指向同一槽

```
✅ 合法：7 号槽在 obj3d 语境 = cascade-0，在 skeleton 阴影 pass 语境 = pose atlas
         （不同 pass，不重叠活跃）
❌ 非法：7 号槽在主 pass 内同时被 cascade-0 和某个新子渲染器的私有纹理占用
```

**检查方式**：静态扫描 `glActiveTexture(GL_TEXTURE<N>)`，
按**所在 pass 函数**分组，断言同一 pass 内 `(N, 语义)` 无冲突。

### R2 — 共享槽必须在 pass 入口绑定，且 pass 内不得覆盖

CSM 级联是全帧只读的共享资源。任何新的子渲染器**不得**在 pass 3 内
`glActiveTexture` 到 `[7, 7+kMaxCascades)` 并 bind 自己的纹理。

### R3 — 独占槽必须"自绑自用自清理"

* 进 draw 前：`glActiveTexture(N)` + `glBindTexture` + `glUseProgram` + `glUniform1i`（**顺序不可换**，§6.2）
* 出 draw 后：active unit 归 `GL_TEXTURE0`；program / VAO / 改过的状态位复原

### R4 — 全帧不解绑共享槽

GL **没有"解绑"语义**。共享槽的正确生命周期是：

```
pass 0 写入 → pass 0 绑定 → 整帧保持 → 下一帧 pass 0 重写时自然覆盖
```

**不得**在帧尾 `glBindTexture(..., 0)` —— 那会让仍认为 sampler 指向该槽的 program
采到未定义内容。

---

## 6. 成本模型（为什么这样分工是划算的）

### 6.1 `glBindTexture` 重复调用 ≈ 免费

对同一槽重复绑同一纹理是**幂等**的，驱动会优化。**因此"每 draw 重绑"不是性能问题**，
可以放心用来换取隔离性。

### 6.2 真正的成本在 program / uniform，不在 texture

| 操作 | 相对成本 | 建议频率 |
|---|---|---|
| `glBindTexture`（同槽同纹理） | ≈0（幂等） | 每 draw 都可以 |
| `glActiveTexture`（同 unit） | 极低（幂等） | 每 draw 都可以 |
| `glUseProgram` | **高**（可能 flush 管线） | **pass 级**，绝不 per-draw |
| `glUniformMatrix4fv` × 5 级联 | 中（80+ floats） | **pass 级**，绝不 per-draw |
| per-object uniform（uMVP/uModel/材质） | 低 | 每 draw 必须 |

**由此得出的分层原则**：

```
帧级      : Shadow pass 渲染 5 级联（一次性）
pass 级   : 绑共享槽 + 声明 sampler 编号 + 传级联 uniform（每个 program 一次，一帧 ≤3 次）
draw 级   : 只做 per-object uniform（uMVP / uModel / 材质常值），不重绑、不切 program
```

**`UploadSunData` 的定位**：它是 **pass 级**函数。`Object3DRenderer::UploadSunData`
一次调用就覆盖 obj3d 的两个 program（内部对 `prog` / `prog_full` 各跑一遍），
`skeleton::UploadSunData` 另调一次。**当前一帧共 2 次调用点**（`renderer.cc:1291`、`1298`），
覆盖 3 个 program。

> ⚠️ **`UploadSunData` 绝不可移入 per-draw 循环。** 重复 `glBindTexture` 无所谓，
> 但 `glUseProgram` + 5×mat4 uniform 上传在 per-draw 频率下会成为真实瓶颈。

### 6.3 关于"绑一次 vs 绑三次"

`UploadSunData` 目前对每个 program 都重新执行 `glActiveTexture + glBindTexture`
（虽然绑的是同一批纹理）。**这不是问题**（§6.1），**也不需要优化**：

* 合并成"全局绑一次"会破坏 pass 级隔离性（退化成依赖"没人覆盖 7..11"的纪律）。
* 理论上的优化是把 `glBindTexture`（全局状态）与 `glUniform1i`（per-program 状态）
  **拆成两个函数**，但收益仅是"一帧少几十次幂等状态调用"，**不值得**。

---

## 7. 现状核对：各子渲染器与 program 数

> 「目前的各个 renderer 应该都是单 shader 的」——**核对结论：近似成立但有一处例外。**

| 子渲染器 | program 数 | program 名 | 说明 |
|---|---|---|---|
| `Object3DRenderer` | **2** | `draw_object3d_pbr`（无 UV 版 VS）/ `draw_object3d_pbr_full`（含 UV+Tangent 版 VS） | **共用同一个 FS** `kMeshFs3dPBR`；按 mesh 是否含 kUV/kTangent 二选一 |
| `SkeletonRenderer` | 1（主）+ 1（阴影） | `skinned_mesh` / `skinned_shadow` | 主 pass 与阴影 pass 各一个 |
| `Primitives3DRenderer` | 2 | `solid3d` / `text3d` | `text3d` 走 `kTexVs3d` + `kText3dFs`；槽 0 绑字形 atlas |
| `SkyRenderer` | 1 | `sky` | **完全不绑定任何纹理**（uInvVP + 标量 uniform 全屏三角形） |
| `FontRenderer` | 1 | `text` | 槽 0 绑字形 atlas |
| `Primitives2DRenderer` | 2 | `solid`（复用）/ `image` | `solid` 与纯色 2D 图元共用；`image` 用槽 0 |
| `Renderer` 后处理 | 7 | `shadow` / `tonemap` / `pick` / `bloom_*`×4 | |

**结论**：**"单 shader"的说法在多数子渲染器上成立**（Sky / Font 各 1 个；
Skeleton 主 pass 1 个）。例外是 `Object3DRenderer` 的 **VS 双版本**——但两版**共用同一个 FS**，
且**槽位布局完全一致**（都是 1..6 材质 + 7..11 级联），所以**对槽位契约无影响**。

> 换句话说：**"多 program"不必然带来"多槽位语义"**。只要两个 program 的槽位布局一致，
> 它们对槽位契约就是同一个消费者。这是当前设计里很干净的一点。

---

## 8. 新增子渲染器的检查清单

- [ ] 不 `#include` 其它子渲染器的头（只依赖 `interface:*` + 三个 manager）。
- [ ] 不依赖其它子渲染器的数据结构（shader 常量各自持一份，宁复制）。
- [ ] 所用槽位在**当前 pass 内**不与任何其它语义冲突（规则 R1）。
- [ ] 若需读 CSM 级联（共享槽），**不得自行绑定**，而是依赖 pass 入口的绑定（规则 R2）；
      必要时扩展 `UploadSunData` 的 program 列表。
- [ ] 每个 `uHas*Tex` 开关都有 `else { ...0 } ` 分支（不靠默认值 / 不靠上一帧）。
- [ ] program 注册名**全局唯一**。
- [ ] 入口 `glPushAttrib`、出口 `glPopAttrib` 成对；active unit 归 `GL_TEXTURE0`。
- [ ] 涉及深度 / 剔除 / 混合 / depthMask / point size 的改动，退出路径显式复位。
- [ ] **绝不把 pass 级操作（`glUseProgram` + 级联 uniform）放进 per-draw 循环。**

---

## 9. 现状缺口与后续建议

| # | 缺口 | 级别 | 建议 |
|---|---|---|---|
| 1 | 槽位表只存在于注释，无单一事实来源 | 🟡 契约缺失 | 把 §3/§4 的表落成 `renderer_texture_units.h` 常量（或至少本文档作为 SSoT） |
| 2 | `kMaxCascades` 上调会撞上 12 | 🟡 脆弱点 | 若要 >5 级联，先把 skeleton 的槽挪出 12 |
| 3 | skeleton 阴影 pass 用 7（与 cascade-0 语义重叠） | 🟢 当前无害 | 按 R1「不同 pass 不重叠活跃」已合法；但应在文档中显式声明，避免后人误以为 7 全帧有效 |
| 4 | 无帧级状态复位基线 | 🟡 | 进 3D 段前统一清 `uHas*`，防新增子渲染器漏置零 |
| 5 | `UploadSunData` 可能被误移入 per-draw | 🟡 | 函数头加"仅 pass 级调用"的显式注释 |

---

## 附：命名约定参考

```
EXCLUSIVE（独占槽）  →  子渲染器自绑自用，不进表，绑完清理 active unit
SHARED   （共享槽）  →  pass 入口绑一次，全帧有效，进表，禁止覆盖
```

判据只有一句：**"这个槽，有没有第二个子渲染器在同一个 pass 里读它？"**
有 → 共享槽；没有 → 独占槽。

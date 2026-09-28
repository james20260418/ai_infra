# JPOV 粒子特效 Pass 设计（火焰 MVP）

> 日期：2026-09-28
> 状态：设计敲定，待实现（MVP 范围 = 火焰）
> 范围：新增一个**并列于 primitive3d 的特效渲染 pass**，MVP 只做火焰，验证接口与目录形态。

## 一、背景与目标

游戏里常见的杂七杂八效果（火焰、烟雾、闪电、雨、雪……）需要一条统一通道。目标是让 JPOV 自持一个「特效杂货铺」——但**先做对，再做全**：

- **MVP 只做火焰**：先把一个效果的「接口形态 / 目录形态 / 渲染姿态 / 深度策略 / 混合策略」全部打通并跑出可看的画面。
- **复用后置**：火、雨、雪、烟雾之间的「复用」被明确定义为**「照抄 shader 代码」级别的复用**，不做「一个 shader 装多个效果」的通用化。等两个以上效果都实现后，再回头抽公共层。

## 二、效果分类（大面三类）

按「**定位空间**」×「**绘制方式**」分类：

| 类 | 定位空间 | 绘制 | 遮挡 | 典型效果 |
|---|---|---|---|---|
| **①** | 3D 空间 | 2D（billboard / 面片） | **有** | 火焰、烟、雨、雪、落叶、电花 |
| **②** | 2D / 屏幕空间 | 2D | 无 | 屏幕雨幕、镜头水滴、全屏闪白、world-anchored UI |
| **③** | 3D 空间 | 3D（真几何） | 有 | 贴地发光圈、火墙、爆炸冲击波 |

**本 pass 服务①类**（3D 定位 + 面片绘制 + 吃 3D 遮挡）。②类挂后处理链，③类走既有 3D 几何管线，不在本 pass 范围。

### ①类内部的进一步区分

- **billboard 粒子**：面片朝向相机（火、烟、电花）。
- **固定朝向面片**：面片法线固定（贴地发光圈法线朝上）——**共用本 pass 的「3D 定位 + 面片 + 混合」能力，但朝向逻辑不同**。

## 三、深度策略

**结论：本 pass 只做最简单的深度。**

- FBO 深度：**复用现有 3D FBO**（`fbo_hdr_` / `fbo_3d_`，均已带 depth texture），**不新建 depth 目标**。
- **测深度**：`glEnable(GL_DEPTH_TEST)` + `GL_LESS` → 被 Object3D 遮挡的粒子正确消失。
- **不写深度**：`glDepthMask(GL_FALSE)` → 半透明粒子之间不互相写深度挡死（对 additive 是期望行为）。
- **不做**：阴影（特效不自阴影）、光照（发光体是 emissive 语义）、depth texture 采样（软粒子属后续）。

> 简单深度是 ①类 99% 场景的完整需求；唯一例外是软粒子（烟贴地软化），属后续增强。

## 四、混合策略

| 模式 | GL | 用途 | 顺序敏感 |
|---|---|---|---|
| **加法 additive** | `GL_SRC_ALPHA, GL_ONE` | 火、电、发光圈、能量 | ❌ 免排序（交换律） |
| **alpha 透明** | `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` | 烟、雾、雨幕 | ✅ 需排序 |

**明确不做乘法调制**：

- **乘法**（`DST_COLOR × ZERO` 之类）的适用场景是「作用于已成画面的调制」（有色玻璃、水下色调、暗角、镜头污渍），**载体是全屏后处理或大面积贴面，不是粒子**。粒子是独立图元，乘法在粒子上的观感是「脏点/黑斑」，不是雾（雾的观感来自 alpha blend）。
- **减法**（`REVERSE_SUBTRACT`）用途极窄，**本 pass 用不到**。

**排序**：alpha 批需要按「离相机从远到近」排序；additive 批免排。排序在 JPOV 内部完成（每帧对实例数组 `std::sort`，规模下开销可忽略）。

## 五、命令接口

命令层**只描述「画什么」，不描述「怎么画」**（不外泄 GL 常量）。

```cpp
enum class ParticleBlend : uint8_t {
    kAdditive,   // GL_SRC_ALPHA, GL_ONE
    kAlpha,      // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
};
```

**vs instancing 的抉择：不做 instancing。**

- instancing 会把接口收窄为「一批同质实例」，**极大牺牲 primitive 的表达能力**（想给某颗粒子单独换纹理/混合/朝向就得拆批）。
- 采用**逐条命令**形态，与现有 `PointLight` 一致（`cmds.point_lights` 逐条、渲染层遍历）。
- 代价：每颗粒子一次 draw。这在 llvmpipe 下真实瓶颈是填充率/overdraw，不是 draw call；**性能问题留给「渲染层内部合并同状态命令」这一后路优化，接口不动**。

## 六、分层：模拟层 → 命令层 → 渲染层

```
OneIteration（有状态：粒子池 / 发射器 / 生命周期）
      │  每帧模拟：按曲线更新 位置/颜色/alpha/尺寸/旋转
      ▼
ParticleCommand × N（无状态：一颗粒子此刻的快照）
      │
      ▼
EffectRenderer（无状态：读命令 → 按 blend 分批 → 忠实绘制）
```

- **有状态的东西（粒子池、生命周期、发射器）住在模拟层**，不在 `RenderCommand` 里（JPOV 是流式、无跨帧资源的）。
- **命令层是「一颗粒子此刻的快照」**，只带视觉原语参数（位置/尺寸/颜色/纹理/朝向/混合）。
- **效果差异 = 参数组合（数据），不是类型**。`ParticleCommand` 里**不出现「火焰」「烟」等业务名**。

## 七、用户接口（效果级，非粒子级）

用户说的是**业务语言**，不是实现语言：

```cpp
// 用户想说的
fire_engine_.AddFire({.position=..., .radius=1.5f, .height=2.0f,
                      .wind={2,0,0}, .density=0.8f});

// 不该外露的（渲染实现细节，禁止交给用户）
ParticleBatchCommand{ texture_id, blend, vertices:[...4000个顶点...] }
```

### 火焰参数清单（MVP 最小可用集，9 项）

业界（Unity ParticleSystem / Unreal Niagara / Godot Fire.shader / CanvasUI FlameWrap）火焰接口收敛出的共识，取 MVP 需要的子集：

| 字段 | 含义 | 来源 |
|---|---|---|
| `position` | 火焰底部锚点 | 通用必备 |
| `radius` | 占地半径 | Danis 点名「多大」 |
| `height` | 向上伸展高度 | Danis 点名「高度如何」 |
| `wind` | 风向 / 风力 | Danis 点名「风向如何」 |
| `density` | 粒子密度（发射率） | Danis 点名「浓度」 |
| `color_core` | 核心色（亮，白/黄） | 业界铁律：火至少 2 色 |
| `color_outer` | 外焰色（暗，橙/红） | 同上 |
| `intensity` | 发光强度（HDR，可 >1） | HDR 发光必需 |
| `speed` | 动画速度倍率 | 手感调节必需 |

**MVP 不做但预留**：`noise`（抖动）、`smoke`（烟量）、`sparks`（火星）、`lifetime`、`radius_gradient`。

> 业界规律：**火焰至少两个颜色**（热色阶），单色必假。这是最容易被漏掉的一条。

## 八、目录结构

```
tools/jpov/effect/
  common/                     ← 薄公共层（先留空，等第二个效果才抽）
    common.h                  // 只放「绝不会因效果而分歧」的东西
  fire_render/                ← 比 object3d 多一层，效果类 renderer 自包含
    fire_render.{h,cc}
    fire.vert / fire.frag     // 火焰自己的 shader
  rain_render/                （将来）
  snow_render/                （将来）
  smoke_render/               （将来）
```

**为什么多一层 `effect/`**：效果类 renderer 归拢一处，与 `object3d/`、`skeleton/`、`primitives3d/` 平级；每个效果自包含一个子目录（货架按品类分）。

## 九、复用策略：「照抄式复用」

**复用 = 照抄 shader 代码 / 渲染器骨架，不是共用同一个 shader。**

理由（效果间是**结构性差异**，非参数性差异）：

1. **顶点路径不同**：火焰 billboard、雨拉长条纹、雪小点摆动——连 vertex shader 都不一样。硬合并 → 每个片段 `if (effect_type == FIRE)` 的分支地狱。
2. **instancing 是顶点路径的分歧**：某些效果后期要上 instancing（完全不同的 per-instance 顶点布局），无法靠 uniform 分叉解决。
3. **与 JPOV 既有哲学一致**：`object3d` / `skeleton` / `skinned_mesh` 本就是各自独立的 renderer + shader，没有「万能 object shader」。

### 薄公共层 + 各效果自包含

「照抄」的代价是**修 bug 要修 N 遍**。解法：

- **薄公共层**：只抽「真正稳定、不会因效果而分歧」的东西（如 `BillboardCorners()`、`ParticleBlend`、深度状态设置）。
- **效果自包含**：效果特有的一切（shader、模拟逻辑、参数解释）各自目录内自包含，可自由抄、自由演化。

**判据**：一个东西该不该抽公共层，只问「**它会不会因某个效果的特殊需求而被迫变异？**」
- 会变异（噪声采样方式：火滚动 vs 雨条纹）→ **别抽，各自抄**。
- 不会变异（billboard 4 个顶点怎么算）→ **抽成薄工具**。

**公共层「先抄后抽、不预判」**：`common/` 一开始可为空；等两个以上效果写完，发现「这段确实每次都一样」，再往上抽。

## 十、渲染挂载点

```
① 3D 不透明（sky → Object3D → SkinnedMesh → Triangle/Strip/Line/Text3D）  测+写深度
② 特效 pass（本 pass）                                                    测深度、不写深度、按 blend 分批
③ highlight pass（现有）
④ tone map（现有 ACES）
⑤ 2D UI（现有，永远最上层）
```

- **②在①之后** → 粒子能被 Object3D 正确遮挡（共享同一张 depth buffer，而非共享 draw call）。
- **②在 tone map 之前** → HDR 的亮（>1.0 的火团）能被 ACES 正确压，不会被 clamp 成死白。
- **HDR 红利**：现有 HDR FBO + ACES（PR #60）本就是发光特效的前提，链路已备好一半。

## 十一、MVP 待抉择（实现前需定）

业界火焰有两种实现哲学，接口形态取决于选哪种：

| 派别 | 形态 | 优点 | 代价 |
|---|---|---|---|
| **粒子派** | 火 = 上百颗粒子，接口偏发射器参数 | 更像「通用粒子底座」，雨雪烟可复用 | 一团火要 1 批 draw（按 blend 分桶） |
| **Shader 派** | 火 = 一个 quad + 滚动噪声 shader，只 2~4 个 uniform | 1 draw call 铁定，参数最少，最像「火焰引擎」 | 不复用粒子体系（雨雪烟另做） |

**倾向**：MVP 用 **shader 派**快速出可看效果，验证 `effect/fire_render/` 的目录/接口形态；等雨雪烟要做时，再回头抽粒子底座。

## 十二、非目标（明确不做）

- ❌ instancing（保表达力，性能优化后置）
- ❌ 乘法调制 blend
- ❌ 阴影 / 光照（特效是 emissive）
- ❌ 软粒子（depth texture 采样）
- ❌ 多效果共用 shader
- ❌ 粒子底座通用化（MVP 阶段）

## 附：本设计讨论结论来源

本设计由 2026-09-28 与 Danis 的九轮讨论收敛而成，关键决策点：
1. 效果按三分类（3D/2D × 绘制方式）。
2. ①类为本 pass 范围；简单深度够用。
3. 不做乘法、不做减法；只 additive + alpha（`GL_ONE_MINUS_SRC_ALPHA`）。
4. 命令层用 `ParticleBlend` 枚举，不外泄 GL。
5. 不做 instancing（表达力优先）；逐条命令，和 PointLight 做一桌。
6. 分层：模拟层（有状态）→ 命令层（无状态快照）→ 渲染层（忠实绘制）。
7. 用户接口为效果级（AddFire…），不是粒子级（顶点数组）。
8. MVP 只做火焰；`effect/fire_render/` 独立自包含。
9. 复用 = 照抄 shader 代码；薄公共层 + 各效果自包含，先抄后抽。

# JPOV 半透明 3D 条带（Strip3D）：廉价玻璃 / 拖尾 / 光环 · 设计

> 日期：2026-10-20 ｜ 状态：**设计（未实现）** ｜ 决策人：Danis
>
> 关联：`jpov_fragment_overdraw_design.md`（WBO 分级纹理）、`jpov_particle_buffs.md`
> （覆盖集 / 加法集）、`jpov_particle_soft_depth.md`（软深度）、
> `jpov_transparent_glass_design.md`（#176 玻璃）。

## 0. 一句话

新增一个与 `ParticleRenderer` 平级的 `StripRenderer`：把「左右双轨 + 逐点 uv/alpha」的
**1D 参数化条带**画进 **1× WBO 目标**（覆盖集 accum/reveal + 加法集 add），
再全屏解析以 over 叠进 HDR。

用途：廉价玻璃（带花纹）、流星拖尾、剑气拖尾、角色光环。

**与三大 3D renderer 的 blend 完全解耦** —— 不动 object3d / instanced / skinned 的任何代码。

## 1. 为什么不做「给三大 renderer 加 blend」

#176 的玻璃走的是 object3d 的 `AlphaMode::kBlend` 子模式：材质级、1× 玻璃 FBO、
还要和 froxel 雾按深度积分 —— 每多一种透明形态都要改主渲染链路的 pass 编排，
而那条路径的 pixel 已经被 gold 锁死。

本设计要覆盖的四类东西（玻璃幕、拖尾、光环）都是**薄片/带状、不受光**的几何，
用不上 PBR / shadow / skinning / tile light。所以正确做法是**独立 primitive + 独立 WBO 目标**：

- 风险隔离：碰不到任何既有 gold；
- 语义清晰：这是一个「未受光的半透明带」，不是「材质」；
- 以后真要更强的玻璃再回去做 #176 的升级，两边互不阻塞。

## 2. 几何

### 2.1 定义

`left_points` / `right_points` **等长**（N ≥ 2），对应点构成一条横跨。
内部交错成 2N 个顶点，按 `GL_TRIANGLE_STRIP` 绘制：

```
L0, R0, L1, R1, L2, R2, ...
```

每个点三个属性：

| 属性 | 类型 | 含义 |
|---|---|---|
| `position` | vec3 | 世界坐标 |
| `uv` | vec2 | 纹理坐标：**u 沿长度（第 i 个点 = i/(N-1)）**、**v 横跨宽度（0 = right / 1 = left）** |
| `alpha` | float | **纵向** alpha 采样值（0..1） |

stride = 6 float = 24 B。

**UV 方向（Danis 2026-10-21 定，与素材对齐）**：图片**从左到右是 s（长度）增长方向**，
**从下到上是 right → left**。四个角因此是：左下 right0、左上 left0、右下 right(N-1)、右上 left(N-1)。
（早期实现把它写反成「u 横跨宽度」，导致纹理转了 90°，已修正。）

### 2.2 宽度参数 w

约定（Danis 2026-10-20）：**右端点 `w = 0`，左端点 `w = 1`**。
`w` 就是上面的纹理 `v`（宽度参数只有一个，别再造第二个名字）。

w 是**参数化坐标**，不是几何距离 —— 用逐顶点常量 0/1 + 硬件插值即可，
不要在 shader 里按位置反算（那会在条带弯曲/透视时漂）。

### 2.3 双面

`glDisable(GL_CULL_FACE)`。

⚠️ **单层四边形关掉剔除不会变成两层**：一个像素只被光栅化一次（三角形要么正面要么背面）。
所以「双面」= 从两边都能看见，**不会**产生 #176 玻璃壳那种「前后各一次」的厚度感。
要厚度请自己错开两条带 —— 写进限制清单。

### 2.4 顶点数上限

每个命令 `2N <= kMaxStripVertices`（建议 **8192**）。

**超限 CHECK 失败，不静默截断**。现有 `Primitives3DRenderer::DrawStrip3D` 的
「截到 3000」（`primitives3d_renderer.cc:205`）是反例：调用方以为画完了，
实际只画了前半截，肉眼看不出来。新接口不复制这个行为。

## 3. 宽度方向的 alpha 剖面

### 3.1 数据：9 个 knot 的分段线性函数

`knots[k]` = 在 `w = k/8` 处的 alpha 乘子（取值 [0, 1]），共 9 个，
把 [0,1] 分成等距 8 段。

FS 求值：

```glsl
float t = clamp(w, 0.0, 1.0) * 8.0;
int   i = min(int(floor(t)), 7);
float f = t - float(i);
float pa = mix(uKnots[i], uKnots[i + 1], f);
```

**必须在 FS 求值**。只在两个端点算好再让硬件插值，plateau 会被抹成三角形。

### 3.2 `Plateau` 语法糖

`StripProfile::Plateau(full_min, full_max)` 生成 knots：区间 `[full_min, full_max]`
内为 1，两侧线性降到 0；端点退化时按单侧处理（`x/0` 的边界单独处理，不允许 NaN）。

**一处判断修正（2026-10-20 讨论中我说错了）**：先前说「plateau 表达不了单侧衰减」，
这是错的。`full_min == full_max == 0` 会退化成 `pa = 1 − w`（右满、左零），
`full_min = 0.5, full_max = 1` 是「左满、右半段衰减」。**单侧衰减 plateau 能表达。**

那为什么还要 8 段通用剖面？为了**非单调**剖面：双软边 + 亮芯、多带花纹、
硬边与软边的任意组合。两者并存：8 段是唯一实现，`Plateau` 只是生成 knots 的糖。

### 3.3 最终 alpha

```glsl
float a = tex.a * tint.a * pa * vAlphaLong;
```

三个乘子都是**逐片元、在 WBO 写入之前**算完 —— 这是 `1 − Π(1−α)` 覆盖率解析成立的前提
（同 `jpov_particle_soft_depth.md §3` 第 1 条：衰减必须发生在权重/覆盖率写入之前）。

## 4. 两个集合：遮罩（覆盖）与发光（加法）

语义同 `jpov_particle_buffs.md §2`：**发光体求和，吸收/遮挡体求覆盖。**

| | `kCoverage`（遮罩） | `kEmissive`（发光） |
|---|---|---|
| 典型用途 | 玻璃、花纹玻璃幕 | 流星拖尾、剑气、光环 |
| att0 accum | `vec4(c·a·w, a·w)` | `vec4(0)`（恒等） |
| att1 reveal | `a` | `0`（恒等） |
| att2 add | `vec4(0)` | `vec4(c·a·gain)` |
| 合成 | `C = accum.rgb/accum.a`，`cov = 1−reveal`，`out = C·cov + dst·(1−cov)` | `out = dst + add.rgb` |

`w = WeightWbo(z, a, c)` **与粒子同一个函数、同一组常数**（`particle_renderer.h:171`，
含 `z/5` 的深度权重）。

> **同一 WBO 目标内必须用同一个权重函数。** 加权平均是 `Σw_k c_k / Σw_k`，
> 混入另一套权重尺度会把平均色整体偏置。

### 4.1 覆盖集 = over 语义，不是逐通道透射（重要）

WBO 覆盖集能表达的只有 `C·cov + dst·(1−cov)` —— 一层**带颜色的膜**。
它**不是**物理玻璃的 `dst·T`（真滤色片：红玻璃把绿蓝吃掉，但红通道几乎不衰减）。

所以本装置的「廉价玻璃」观感 = 半透明有色膜（与 #176 玻璃同一视觉模型），
不是真滤色片。

**好处**：永远不需要 `max(rgb)·α` 那种伪消光 —— 那种写法把「颜色权重」当成了「遮挡」，
后果是暗花纹几乎不挡光、亮花纹反压背景、HDR 下 `rgb.max > 1` 还会让透射率为负、颜色反相。
WBO 里消光**只由 α 决定**，`max(rgb)` 只进颜色加权平均。

若将来确实需要逐通道吸收，那是一条独立的**乘法路径**（直接画进 HDR +
`glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA)`，`src = (T·a, a)`），
不在本设计范围内，单独讨论。

### 4.2 集合内部的顺序无关，集合之间有序

- **集合内部**：`accum +=` 是加法、`reveal *=` 是乘法 ⇒ 绘制顺序无关。
  这是 WBO 的全部价值所在（免排序）。
- **集合之间**：先覆盖集、后加法集 ⇒ 加法光**不被**遮罩衰减。
  这与粒子现状一致（`particle_renderer.cc:322-336`，`jpov_particle_buffs.md §2.3` 的已知近似）。
- 反过来（先加后遮）会让加法光被遮罩衰减 —— 那是「烟在火前」的语义，两者只能选一个，
  除非做 per-pixel 深度的 MBOIT。

**这个决策写死在 `Composite()` 里，不暴露给调用方**，避免有人靠调整 draw 顺序改变画面。

## 5. WBO 目标与合成

### 5.0 用共享件，不要手抄（2026-10-20 重构后）

WBO 的 FBO / 附件 / clear / 混合 / 还原已经收敛成 `src/wbo/wbo_target.{h,cc}` 的 `WboTarget`，
权重与解析收敛成 `src/wbo/wbo_shader.h`。**条带必须用它们**，不要复制粘贴一套新的。
规则见 `docs/jpov_wbo_target.md §3`。要点：
- 只申请要读的附件（条带 v1：accum/reveal + additive；不要 depth）；
- `BindForDraw()` / `EndDraw()` 包住绘制；
- 权重从 `kWboWeightGlsl` 注入；
- 解析用 `kWboCompositeFs`。

### 5.1 一个 FBO、三个附件（1×）

```
COLOR0  accum   RGBA16F   Σ(w·c)
COLOR1  reveal  R16F      Π(1−α)
COLOR2  add     RGBA16F   Σ(c·a·gain)
```

尺寸 = `fbo_3d_w / fbo_3d_h`（**1×**）。

> **术语澄清**：粒子那个「add 一个 / over 一个」其实是一个 FBO 的三个附件
> （`particle_renderer.cc:110-125`），不是两个 FBO。
> **几个 FBO = 几个层；几个 attachment = 几个集合。**

为什么是 1×：粒子降到 1/2 是为了省填充（sprite overdraw），
但花纹玻璃的纹理细节在 1/2 下会糊；条带的填充面积本身很小，不值得降采样。

### 5.2 层序（跨 WBO 目标是有序的）

当前透明层与合成点：

| 层 | 目标 | 分辨率 | 合成点 |
|---|---|---|---|
| 粒子 | `ParticleRenderer::wbo_fbo_` | 1/2 | 不透明 3D 之后 |
| 体积火 | `VolumeFireRenderer::wbo_fbo_` | 1× | 粒子之后 |
| **条带（本设计）** | `StripRenderer::wbo_fbo_` | 1× | 体积火之后 |
| 玻璃 #176 | `Renderer::glass_fbo_` | 1× | froxel 雾合成内 |

固定顺序：**粒子 → 体积火 → 条带 → 远景雾 → froxel 雾/玻璃**。

理由：条带是最靠前的「手工效果层」，且要让它继续被雾按场景深度作用
（合成在雾 pass 之前，雾读的仍是该像素的不透明场景深度）。

**为什么不和体积火共用同一个 FBO**：体积火是单层、权重恒为 1（`accum = vec4(L, α)`），
条带用 `WeightWbo` 的深度权重。混在一个 accum 里，加权平均会被两套权重尺度偏置。
统一权重会改体积火的 gold —— 不值得，所以分开。代价是多一趟全屏合成。

### 5.3 为什么用 WBO 而不是直接 over 进 HDR

直接 `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` 画进 HDR 更便宜（0 个全屏 pass）。
选 WBO 的三个理由：

1. 多条带 + 玻璃幕互相重叠时，over 的结果**依赖绘制顺序** —— 调用方要排序，
   而这正是 WBO 要消灭的东西；
2. 加法光与遮罩光的先后需要一个统一决策点，散在 draw 顺序里就无法保证；
3. 与粒子同一套代数，以后可以收敛成共享模块。

**代价**：每帧固定 3 张全屏 float 纹理 clear + **2 趟全屏合成**
（1280×720 ≈ 1.8M fragment），即使条带只覆盖屏幕 5% 也要付。
llvmpipe（gold / CI 环境）上这笔钱更明显。

**退化路径（留好口子）**：条带的 FS 只依赖 `scene_depth` + 纹理，不依赖 WBO 目标结构，
所以「直接画进 HDR + 硬件深度测试」是一条随时可切的旁路。若实测成为瓶颈再做。

## 6. 深度与软深度

WBO FBO **没有深度附件**，所以：

```glsl
float a = tex.a * tint.a * pa * vAlphaLong;   // §3.3 的最终 α
// 手动作深度测试 + 软深度（必须在写 WBO 之前）
vec2  uv      = gl_FragCoord.xy / uViewport;
float scene_z = ZView(texture(uSceneDepth, uv).r);
float z       = ZView(gl_FragCoord.z);
a *= clamp((scene_z - z) / uSoftDepthM, 0.0, 1.0);
```

- **直接复用 `ParticleRenderCommand::soft_depth_m` 的语义与参数名**（`d = scene_z − z`，米；
  `0 < d < m` 内线性淡出，`d <= 0` 丢弃）。
- **默认必须给值，不提供 `nullopt` 硬判据**：1× 画质下那条刀切线比粒子明显得多
  （剑气扫过地面、流星穿过建筑、光环插进角色身体）。等真有 A/B 需求再开放。
- 衰减必须乘在 `WeightWbo` 与两个集合的输出**之前**（同 §3.3）。
- 纹理单元是 pass-local：`unit0 = scene depth`、`unit1 = 条带纹理`
  （与粒子一致，`particle_renderer.cc:33-34`）。不复用主链路的
  `kTexUnitMaterialBase` 区间 —— 本 pass 不跑 object3d。

## 7. 着色（unlit）与 fresnel

> 实际落地版本见 **§19**：参数是 `StripFresnel`（批次级 optional），法线按 pass 分两种
> （kSmooth 用解析法线、kPolyline 用导数法线），且边反作用在覆盖集的**色与 α**上，
> 不是本节草图里的「另写加法集」。

条带**不受光**（同 `Primitives3DRenderer` 的纯色 shader）：没有 PBR、没有 shadow、
没有 tile light。这是有意的 —— 目标是廉价。

但「廉价玻璃」没有高光就完全没有玻璃味，所以补一个几乎免费的 fresnel：

```glsl
vec3  N   = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
float rim = pow(1.0 - abs(dot(N, normalize(uCamPos - vWorld))), uFresnelPower);
// rim 注入**加法集**：vec4(rim_color * rim * strength, 0)
```

- `abs()` 处理双面（法线方向逐面翻转，正反面都成立）。
- 默认 `fresnel_strength = 0`（关）；玻璃命令建议 0.3~0.8，`fresnel_power` 默认 4。
- 法线来自屏幕空间导数 ⇒ 条带越细法线越抖，掠射角可能闪。
  若闪得厉害，退化方案是几何上传法线（+3 float/vertex）。先不加。

**一条命令可以同时写两个集合**：覆盖集写「膜」，加法集写「fresnel 边缘反射」。
不用的附件写恒等值（accum=0、reveal=0、add=0），这也是粒子 FS 的写法。

## 8. 命令与接口（M1 定稿，2026-10-21）

> **方案改变**：原设计是「任意长度双轨 + instancing 上 GPU 展开」。与 Danis 讨论后改为
> **固定 16 点 + CPU 展开 + 整批一次 draw call**，理由见 §16.1。下面是**实际落地**的接口。

```cpp
// 一条带：16 个采样点的双轨（POD，可整块 memcpy）
struct Strip16 {
    std::array<Vec3f, 16> left;    // 左轨（纹理 u = 1）
    std::array<Vec3f, 16> right;   // 右轨（纹理 u = 0）
    std::array<float, 16> alpha;   // 纵向 alpha 乘子 [0,1]
    uint32_t point_count = 0;      // 有效点数 [2,16]，必须显式给
};

// 一批条带：以下参数整批共享 ⇒ 一次 draw call
struct TranslucentStripBatch {
    uint32_t texture_id = 0;               // 0 = 纯色
    Color tint{1, 1, 1, 1};                // rgb 与纹理相乘；a 全局不透明度
    StripBlend blend = StripBlend::kCoverage;   // 覆盖集（吸收）/ 加法集（发光）
    std::optional<float> soft_depth_m;     // nullopt = 硬判据；有值必须 > 0（米）
    std::array<float, 9> width_profile = {1,1,1,1,1,1,1,1,1};   // v = k/8 处乘子
    std::vector<Strip16> strips;
};

cmds.DrawTranslucentStrips(std::move(batch));
```

**几何约定**（`src/strip/strip_geometry.h` 唯一实现）：

- 每条带展开成 32 顶点 = 16 列 ×（right, left），15 个 quad / 90 索引；
- `right[i] → uv = (0, i/15)`、`left[i] → uv = (1, i/15)`（u 横跨宽度，v 沿长度）；
- `point_count < 16` 时，第 `col >= point_count` 列**塌到最后一个有效点**（零面积 → 不产生片元），
  但 **v 仍按列号**（`col/15`）⇒ 短条带只用纹理前一段，图案不被拉伸；
- 整批共享一个**静态索引缓冲**（1024 条带/块），超过才分块多次 draw（**不截断**）。

**Pre-condition（绘制前 CHECK，不静默容错）**：

- `strips` 非空；
- 每条 `point_count ∈ [2, 16]`；
- 有效点的 left/right 坐标有限（NaN 进 HDR 会污染整条后处理链）；
- 有效点 `alpha ∈ [0,1]` 且至少一个 > 0（防 `memset(0)` 的整条隐形）；
- `width_profile` 9 个 knot ∈ `[0,1]` 且至少一个 > 0；
- `tint` 各分量 ∈ `[0,1]`。

**不加 `picking_id`**：本版不参与拾取（同玻璃现状），先不暴露「设了但不生效」的字段。
`blend` 与 `soft_depth_m` 已随 M2/M3 落地（见 §16.6/§16.7）；
`fresnel_*`（M4）**等落地时再加**，不预留空字段。（→ 已于 2026-10-22 落地，见 §19）

## 9. 四个用途的适配

| 用途 | blend | 关键旋钮 | 备注 |
|---|---|---|---|
| 廉价玻璃 | `kCoverage` | 纹理（花纹）+ `tint.a` 控不透明度 + fresnel | 硬边用全 1 profile；观感=有色膜（§4.1） |
| 流星拖尾 | `kEmissive` | 宽度剖面 + 逐点 `alpha`（头 1 → 尾 0）+ `additive_gain` | 宽度收窄是**几何**（左右点靠近） |
| 剑气拖尾 | `kEmissive` | `Plateau(0, f)` 单侧 + 逐点纵向衰减 + `additive_gain` | 扇形是**几何**（扫角 + 半径） |
| 主角光环 | `kEmissive` | 纹理环向 repeat + 径向剖面 + 可选 fresnel | 左右点 = 内/外半径；UV 沿环向 |

## 10. 限制（写清楚，别让调用方猜）

1. **1D 参数化曲面**：只能表达左右两条曲线张成的带。L 形玻璃、球壳、
   任意多边形面板都要拆成多条。
2. **单层没有厚度**：双面 ≠ 前后两层（§2.3）。
3. **unlit**：不参与光照 / 阴影 / 拾取 / highlight；雾按该像素的**场景深度**作用，
   不是条带自身深度。
4. **层序固定**：条带永远在粒子 / 体积火之后合成；与它们之间的真实前后关系不做逐像素整合。
5. **加法光不被遮罩衰减**（同粒子）。
6. **覆盖集不能做逐通道吸收**（§4.1）。
7. **分辨率固定 1×**：本版不做 1/2、1/4 分级路由。

## 11. 性能

| 项 | 成本 |
|---|---|
| clear | 3 张全屏（RGBA16F ×2 + R16F） |
| 绘制 | 2N 顶点 strip，1× 填充（只有条带覆盖面积） |
| 合成 | 2 趟全屏（仅当对应集合有内容） |
| vs 直接画 HDR | 多 2 趟全屏 + 3 次 clear，换来免排序与统一代数 |

空集合跳过合成（`has_content_` 模式，同 `particle_renderer.cc:299`）。
`Begin()` 的 clear 只有在「本帧确实有条带命令」时才跑 —— 与玻璃 FBO 的按需分配一致
（`renderer.cc:1741`）。

## 12. 测试计划

1. **gold：覆盖集** `strip_glass_1280x720.png` —— 花纹玻璃条带压在有红/白背景的盒子上，逐字节。
2. **gold：加法集** `strip_trail_1280x720.png` —— 流星拖尾，逐字节。
3. **非恒真断言**（防「参数退化 + 重出 gold」一起绿）：
   - 沿宽度采样：`alpha(w=0) − alpha(w=1) > 0.5`（单侧梯度必须存在）；
   - 软深度：几何相交处的半透明像素数 ∈ `[N_min, N_max]`（硬判据下 ≈ 0，能区分）；
   - 加法集：亮部像素数 ∈ `[X, Y]` 且 `sum(add.rgb) > 0`。
4. **A/B**：`soft_depth_m` 大/小两张图，差异像素数 ∈ `[1000, 20000]`
   （同时防「根本没生效」和「整屏都变了」）。
5. **负向**：`left.size() != right.size()` 必须 crash（death test）。

> 另：WBO 权重函数的文本契约测试（`test/wbo:wbo_weight_contract_test`）已随配套 WBO 重构落地，
> 条带沿用同一份权重，无需额外测试。

## 13. 与现有代码的关系（2026-10-20 重构后已更新）

**已落地（配套 WBO 重构 PR）**：
- `WboTarget` 接管粒子与体积火的 FBO/clear/blend/还原（`docs/jpov_wbo_target.md`）；
- 权重函数与全屏解析共享化（`kWboWeightGlsl` / `kWboCompositeFs`），并有文本契约测试守 drift；
- **体积火的死 depth 附件已删**（`FragDepth` 输出 + 第三附件 + num/den 累积一并移除），gold 逐字节不变；
- 槽位约定定为 `0=accum / 1=reveal / 2=additive / 3=depth`。

**未迁移（有意）**：
- 玻璃 FBO 仍由 `Renderer` 手写：它要把 `FragDepthAccum` 挪到槽位 3，且现有 gold 只有
  「8×8 块平均差 ≤ 30」太松，迁移前得先收紧到逐字节。见 `jpov_wbo_target.md §5`。

**条带接入时（新增内容）**：

- 新增 `src/strip/strip_renderer.{h,cc}`：`Begin()` / `Draw()` / `Composite()`，
  结构对齐 `ParticleRenderer`。
- `interface/render_command.h`：加命令结构 + `RenderCommandList::DrawTranslucentStrip3D()`。
- `src/renderer.cc`：在体积火之后、horizon fog 之前调用。
  **合成时必须 `glDrawBuffers(1, {COLOR_ATTACHMENT0})` 再恢复 MRT(2)** ——
  否则 MRT#1 的场景深度附件内容未定义，会出现 PR #176 那个 tile 级逐帧闪烁。
- 旧 `Strip3DCommand`（纯色调试线）**保留不动**。

## 14. 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | WBO 准备：`WboTarget` + 共享权重/解析 + 删死 depth + 契约测试 | ✅ 已进 main（#209） |
| M1 | 命令 + 几何 + 剖面 + 覆盖集 + Composite + gold + viewer | ✅ 2026-10-21（§16） |
| M2 | 加法集 `kEmissive` + gold（拖尾） | ✅ 2026-10-21（`additive_gain` 未做：亮度直接由 tint/纹理控制） |
| M3 | 软深度（per batch `soft_depth_m`）+ A/B 断言 | ✅ 2026-10-21 |
| M4 | fresnel（+ 条带圆环；法线：kPolyline 导数 / kSmooth 解析） | ✅ 2026-10-22（§19） |
| M5 | viewer 扩到四用途同屏 | ⬜（M1 已带 3 形状 viewer） |

## 15. 不在本设计范围

- 逐通道吸收的「真玻璃」（另一条乘法路径，见 §4.1）
- 多分辨率分级（1/2、1/4）
- MBOIT / 逐像素深度整合
- 拾取 / highlight / 阴影 / 光照

## 16. M1 实施记录（2026-10-21）

### 16.1 为什么最终**没有**用 instancing

讨论中曾计划「per-instance 属性 + VS 展开」，结论是**不需要**：

| instancing 声称省的 | 实际情况 |
|---|---|
| 每实例的 draw call / uniform / 状态 | ✅ 但「CPU 展开 + 整批一次 draw」同样能省，且少四层机制 |
| 顶点数据上传量 | ❌ 形状每帧变，数据本来就要传；16 点 vs 32 顶点量级相同 |
| VS 做展开、CPU 不建顶点 | ❌ 16 点算完写到 32 个顶点是免费操作 |

真正的拦路虎是：**GLSL 顶点属性不支持动态下标** ⇒ 「VS 里按列号取 left[i-1]/left[i+1]」
必须先有可索引的存储（数据纹理 + `texelFetch`），而法线砍掉后这件事根本不需要做了。

结论：`StripBatch` 是稳定的**语义**边界（同材质 = 同批），内部实现（CPU 展开 / 将来实例化）
对调用方不可见。

### 16.2 法线：不做顶点法线

条带是平面片，真实法线 = 所在三角形平面法线 = `normalize(cross(dFdx(worldPos), dFdy(worldPos)))`。
顶点法线反而是"平滑谎言"，会让 fresnel 在轮廓处算错。M1 无 fresnel（M4 再加），
所以 VS 里连法线都不算。

### 16.3 UV 方向（2026-10-21 修正）

第一版把 UV 写反了（u 横跨宽度、v 沿长度），素材整体转了 90°。
**正确约定**：u 沿长度（图片从左到右 = s 增长）、v 横跨宽度（0 = right / 1 = left），
四角 = 左下 right0、左上 left0、右下 right15、右上 left15。
由 `strip_geometry_test` 的 `ExpandsFixedTopology` / `CollapsesTailBeyondPointCount` 精确断言。

### 16.4 素材坑：不要给「色带测试卡」开 mipmap

`assets/strip/ribbon_test.png`（1318×158，横向 4 条色带）在条带上 u 方向被压缩上百倍，
mip 会把**纵向**相邻色带平均成灰色（实测：红/黄/青全被冲淡成灰粉）。
色带在 u 方向本是常量，关掉 mipmap 后颜色才正确。**换成真正的花纹素材时要重新评估**
（那时 u 方向有细节，就不能无脑关 mip）。

### 16.5 验收

- `bazel test //tools/jpov/test/strip:all` → 13 cases（含 6 个 `EXPECT_DEATH` 负向用例）；
- gold `test/strip/strip_demo_1280x720.png`（三条带 + 天光 + 三块参考方块），逐字节；
- 非恒真断言：带纹理 vs 纯色差异 36,372 px ∈ [20k,120k]；差异区内红 4,410 / 青 4,714 /
  黄 4,210 px 各自在区间内；柔边 vs 硬边 19,736 px ∈ [5k,80k]；
- 「一次 draw call」证据：`StripRenderer: strips=3 draw_calls=1` 日志（像素不可测）。

**查看器** `tools/jpov/build_jpov_strip_viewer.sh` → `output/jpov_strip_viewer/jpov_strip_viewer`：
右键拖拽绕行 / 滚轮推拉；面板可调 太阳仰角、不透明度、条带宽度、波带振幅、螺旋转数、
弧形开角/朝向、纹理 vs 纯色、柔边/hard、形状动画、自动绕行、SMAA、三条带各自显隐、A/B 显示条带。
`--capture <dir>` 输出 5 张固定视角对照图（默认 / 无条带 / 纯色 / 硬边 / 面板）。

### 16.6 加法集（M2）

`StripBlend::kEmissive`：FS 只写加法集 `att2 = 色·α·fade`，覆盖集写恒等值；
Composite 先 over 覆盖集、再 `(ONE,ONE)` 求和加法集。

- **附件按需申请**：`Begin(w, h, with_additive)` 由 Renderer 扫一遍本帧有没有
  `kEmissive` 批决定；只有覆盖批时不建 att2（省一张 1× RGBA16F 的 clear）。
- 覆盖批在「带加法集的目标」上照常画（att2 写 0 = 恒等），所以**两种批可以共存于同一帧**，
  但因为是不同附件/不同 pass，必须拆成两个 batch（= 两次 draw call）。
- 断言：加法相对覆盖**只允许变亮**（实测 darkened = 0，差异 5,939 px）——
  出现变暗像素就说明实现退化成覆盖集或写错了附件。

### 16.7 软深度（M3）

逐 batch 的 `soft_depth_m`：`nullopt` → 硬判据（比场景深 2cm 即丢弃）；
有值 → `fade = clamp((scene_z − z)/值, 0, 1)`，衰减**发生在写目标之前**，
所以覆盖率解析仍然自洽。

- 场景里特意放了一块「穿过波带的方块」，否则软深度是**空测**（条带不与任何几何相交时
  A/B 完全一致 —— 第一版就是这样，差异 0 px，靠断言才发现的）。
- 实测：hard vs soft 差异 571 px（集中在穿模处）。

## 17. 性能：每帧成本与「已前置」的运算（P8 review，2026-10-21）

### 17.1 每帧成本结构（按量级）

| 环节 | 复杂度 | 说明 |
|---|---|---|
| 校验 `ValidateStripBatch` | O(点数) | 逐点有限性 + α 范围；约 48 次比较/条带 |
| 展开 `ExpandStripVertices` | O(点数) | 32 顶点 × 24 B = 768 B/条带 写入 |
| 上传 | O(顶点) | 每个 draw 块一次 `glBufferData`（orphan） |
| 绘制 | 一次 `glDrawElements` / 块 | ≤ `kMaxStripsPerDraw`(1024) 条带 |
| 填充 | O(覆盖像素) | **通常是真正的瓶颈**：1× 分辨率 + 加法集重叠 |

1000 条带 × 16 点：约 768 KB 顶点数据/帧、32k 顶点、1 次 draw call。

### 17.2 已经前置的运算

- **索引模式**：`Init` 里按 1024 个固定块（每块 90 索引）一次性建静态 IBO，
  运行时零索引计算；
- **每列 UV 的 u**：`kStripColumnU` 编译期常量表（不再每帧每列做除法）；
- **uniform 位置**：`Init` 时查一次缓存进 `DrawUniforms` / `CompositeUniforms`。
  之前每批调 12 次 `ShaderManager::GetUniform`，而它对每次调用都**线性扫一遍
  programs_ 再算一次 std::string 哈希**（`shader_manager.cc:148`）——
  1000 批就是 12000 次字符串哈希/帧，纯浪费；
- **顶点缓冲容量**：`vertices_.reserve(kMaxStripsPerDraw × 32)`，
  避免首帧/扩容时的 vector 重分配（VBO 本身仍每帧重传）。
- **WBO 附件按需申请**：没有 `kEmissive` 批时根本不建 att2（省一张 1× RGBA16F 的 clear）。

### 17.3 有意**没有**做的优化（及原因）

1. **不用 instancing / 数据纹理**：见 §16.1 —— 在「16 点固定 + 无法线」的前提下，
   CPU 展开 16 点几乎免费，省下的只有 draw call，而 draw call 已经被「整批一次」解决。
2. **不用 `glBufferSubData` 复用同一块 VBO**：需要预分配 + 复用；GPU 读上一帧内容时
   会产生同步等待。`glBufferData` 会 orphan 旧存储，天然规避（代价是每帧一次分配）。
3. **不给 FS 加纹理/纯色的 program 变体**：`uHasTex` 是**整批一致**的 uniform 分支，
   没有 divergence；拆变体只省一条分支，换来program 数量翻倍 ⇒ 不划算。
4. **不做宽度剖面的 LUT 纹理**：9 knot 的动态下标在桌面 GL 上就是常量缓冲寻址，
   实测没有可观开销；换成纹理反而多一次采样。
5. **`discard` 保留**：WBO 目标没有深度附件，`discard` 不承担 early-Z 语义，
   只用来跳过「α≈0」片元的后续计算（写恒等值等价，但 `discard` 更省填充）。

### 17.4 若将来要再提速，优先级

1. **降低 overdraw**（真瓶颈）：按屏幕 tile 剔除、或对加法集做 1/2 分辨率降采样；
2. **CPU 展开 → 期望式批量写**：把 32 顶点写入改成直接写映射内存（`glMapBufferRange`），
   省掉 vector→GL 的一次拷贝；
3. **多材质合批**：同尺寸纹理做 2D array + per-batch layer，把「一材质一 draw」
   收敛成「一次 draw」。


## 18. 平滑 pass（kSmooth）—— 2026-10-21

### 18.1 为什么有两个 pass

固定 16 采样点时，折线段与段之间是**弦**，弯处能看到多边形。加采样点是最直接的办法，
但把「点数」变成了用户的负担。于是提供两种绘制风格，**同一个命令接口、同一张 WBO 目标**：

| 风格 | left/right 的含义 | 段间 | 成本 |
|---|---|---|---|
| `StripStyle::kPolyline`（默认） | **顶点** | 直线（弦） | 最低，顶点 6 float |
| `StripStyle::kSmooth` | **插值型控制点（knot）** | C1 Hermite 曲线 | 顶点 7 float + 每条带 768 B 数据纹理 |

**物理契约对两种风格完全相同**（用户不需要为切换改任何东西）：

1. 曲线/折线**严格经过**每一个 `left[i] / right[i]`；
2. 第 i 列宽度**严格等于** `|left[i] − right[i]|`，段间由半宽矢量插值决定；
3. 想改宽窄 → **移点**（不存在 width / widen 参数）。

### 18.2 片元里做了什么

每个片元只属于**一个段**，只读该段**两列**的控制数据（6 个 `texelFetch`），求值一次
三次 Hermite，得到真实中心线 `S(t)` 与半宽矢量 `H(t)`，然后：

1. **反解参数**：`Linear(u,v) = S(u,v) + D ⇒ (t*,v*) ≈ (t,v) + J⁻¹D`，
   纹理与宽度剖面改用 `(u*,v*)` → 图案跟着曲线走，不在接缝处"折一下"；
2. **裁形**：`v*` 落在 `[0,1]` 之外 → `discard`（Loop-Blinn 那套的裁形思想），
   用 `fwidth(v*)` 做 1 px 解析 AA → 轮廓不再是多边形，也不需要 MSAA/SMAA。

**复杂度 O(1)**：与采样点数 m 无关（m 只影响 CPU 打包 O(m)、顶点数 2m、纹理行宽 3m；
shader 里连 `m−1` 都换成 uniform `uSegCount`）。

### 18.3 外扩量是自动的（不是参数）

片元只能"削"不能"补"：真实曲线在弦**外侧**鼓出的部分没有片元可画。所以先把多边形按
**局部曲率最大的外凸量**撑大，再由 FS 用 `v*` 裁回去。

- 外扩量按**绝对量（米）**算，不是比例 —— 拖尾收窄到 0 的段落按比例会失真；
- 每条带取所有段、7 个采样点的最大值，再 ×1.02 + 1 mm（采样是最值近似，留余量）；
- 实测（demo）：波带 3.0 mm（半宽 35 cm）、螺旋 **58 mm**（半宽 22 cm）、弧形 8.1 mm。
  螺旋那 58 mm 就是 `R(1−cos(Δθ/2))` 的 sagitta，也说明**固定比例 1.12（=26 mm）根本不够**。
- 外扩量**折进顶点属性 v**（`v = 0.5 ∓ scale/2`），所以 FS 里没有任何 widen 参数/额外 fetch。
  ⚠️ **scale 必须是整条带一个值**：FS 假定 `position = lerp(c) + (2v−1)·lerp(h)`，
  只有 scale 恒定才有 `lerp(scale·h) == scale·lerp(h)`；逐列不同会在**收窄**的条带上
  破坏映射，露出亚像素缺口（P8 review 实测：A/B 图上表现为毛边）。scale 按**最细**那一列算，
  上限 8×（更细的尖本来就亚像素，不值得铺满屏幕）。

### 18.4 数据通道与代价

- 控制点走 **RGBA32F 数据纹理**：每列 3 texel = 12 float `(c.xyz,m.x)(m.yz,h.xy)(h.z,n.xyz)`，
  每条带占一行；顶点只多一个 `aStripId`（7 float，28 B）⇒ **整批一次 draw call**。
- 1000 条带/帧：顶点 896 KB + 纹理 768 KB，**draw call 恒为 1**（不分块：
  数据行号 = 全局 strip 下标，分块就要么重传顶点、要么写一张正在被采样的纹理）。
  单批上限是数据纹理行数（`GL_MAX_TEXTURE_SIZE`，常见 16384），超了直接 CHECK 崩。
- FS：6 次 `texelFetch`（每片元 96 B）+ ~140 ALU，无分支发散。
- 与「每条带一套 uniform」的早期版本**逐像素一致**（6 张 capture maxdiff=0），
  但 draw call 从 N 次回到 1 次。

### 18.5 已知限制

- 两条轨**各自插值**：契约是"中心线 + 半宽矢量都 C1"，不是"两条独立轨道各自 C1"。
  若用户给的两轨形状差异极大（比如一侧有尖锐折角），平滑 pass 会把折角也磨圆 —— 这是
  选择 kSmooth 的固有语义。
- 内弯侧会**比折线 pass 略窄**：折线 pass 的弦在真实轨道外侧（画胖了），平滑 pass 裁回真实宽度。
- 外扩倍数有 **8× 上限**：一个条带上「最弯处的外凸量 / 最细一列的半宽」超过 7 时撑不满，
  极细的弯尖外侧仍可能缺一条（亚像素级）。这是**有意的取舍**（为一条亚像素的尖把整条带
  撑到屏幕外没有意义），不告警。
- 法线来自屏幕空间导数（`dFdx/dFdy`），条带极细时 fresnel（M4）可能闪 —— 与本节无关。

### 18.6 viewer 验收

`jpov_strip_viewer` 面板新增「平滑条带（C1 重建）」勾选框，实时切换两个 pass；
`--capture` 会额外输出 `70/75/80_smooth_*` 三张对照图（同一视角/开关，只换 style），
与 `60_zoom_soft` / `65_zoom_hard` / `10_default` 一一对应。

### 18.7 复核修正（2026-10-22）

第三轮 review 后修的三处（都会造成「出图看不出来、但确实在漏画/变淡」的问题）：

1. **外扩量不能再按「本段等宽就跳过 Hermite」简化**。FS 恒算完整 Hermite
   （含 `h10·n0 + h11·n1`），而切线只看相邻列 —— 局部等宽、隔壁列在收窄（"肩部"）时
   `n ≠ 0`，简化会让 margin 低估。实测：半宽 0.2 的直条带、末列收到 0，两侧各缺 4.25 mm。
   现在 CPU 恒用完整 Hermite（成本可忽略），并由单测做「margin ≥ 密采样真实外凸量」断言。
2. **两端切线用完整单侧差分**（`p1 − p0`），不是 `0.5·(p1 − p0)`。后者会让每段条带的
   首/末段参数化被压缩（16 点直条带端段偏 5.5% 段长、2 点条带偏 4%），纹理被不均匀拉伸，
   而且「2 点 kSmooth == kPolyline」这个数学事实不成立。
3. **边缘 AA 的覆盖度是「到最近边的距离」**：`cov = clamp(0.5 + min(v*,1−v*)/fwidth(v*), 0, 1)`。
   旧写法把两个边各自的 0..1 覆盖度**相乘**，条带整宽 < 2px 时中段被平方衰减
   （1px 宽只剩 0.25）⇒ 掠射角/远处提前淡出。

配套：这些数学全部搬进 `strip_geometry.h`（`ComputeSmoothStrip` /
`AppendSmoothStripData` / `AppendSmoothStripVertices`，GL-free），
`test/strip/strip_geometry_test.cc` 有 10 条断言，其中 3 条是**负向对照**
（把旧实现算回来必须失败），防止「参数退化 + 断言恒真」一起绿。

**结构收口**（同一轮，纯重构，出图逐像素不变）：

- 片元着色器拆成 `kCommonFsHead`（uniform / varying / 工具函数）+ `kWboWeightGlsl` +
  `kFsEmit`（深度测试 / 两个集合 / WBO 写入）+ `kFsMain` | `kSmoothFsMain`。
  两个 pass 的「最后一公里」从此只有一份 —— 此前是逐字复制的两段。
- C++ 侧：`QueryCommonUniforms(program, out)` 是公共 uniform **名字表**的唯一一份，
  `ApplyCommonUniforms(uniforms, ctx)` 是**上传**的唯一一份，
  `BatchDrawContext` 把 drawer 原先的 7 个位置参数收成一个结构体。
- viewer 的 `--capture` 改成**拍摄表**（每行只写与默认值不同的开关），删掉手动还原；
  同时删掉一张与 `75_smooth_zoom_hard` 完全相同的重复图（`85_smooth_zoom_hard_alt`）。

## 19. M4：菲涅耳「廉价玻璃」+ 条带圆环（2026-10-22）

Danis 的原话：「这次我们做 frensel 那个玻璃质感的事情，首先可以用 strip 围个圆环出来：
上下各一层圆圈，侧面内外各一圈立着的 strip。就这样，应该就四组点：up_outer, up_inner,
down_outer, down_inner，一组合就能出这个圆环的 4 个 strip。圆环放在 strip viewer 里。」
于是 M4 和「一个能看出边反的玻璃壳」一起做。

### 19.1 圆环 = 四组点 → 四条带

16 点拓扑天生就是「一圈」：16 列里第 15 列与第 0 列重合 ⇒ 一圈 15 段，天然闭合。
所以四组点（上/下 × 内/外，各 16 个）就能拼出圆筒外壳的四条带：

| 带 | left（v=1） | right（v=0） | 长度方向 | 宽度方向 |
|---|---|---|---|---|
| 顶环面 | `up_outer` | `up_inner` | 绕圈 | 环宽 |
| 底环面 | `down_outer` | `down_inner` | 绕圈 | 环宽 |
| 外立壁 | `up_outer` | `down_outer` | **绕圈** | 高度 |
| 内立壁 | `up_inner` | `down_inner` | **绕圈** | 高度 |

⚠️ 最容易想错的一处：**立壁的「长度方向」也是绕圈，不是高度**。把高度分 16 段就凑不出
闭环（四条带的列数也对不上）。这也是为什么四条带能塞进同一个 batch —— 它们共用同一份
16 列参数化，只是一次 draw call 里的 4×32 个顶点。

实现在 `src/strip/strip_geometry.h`：`StripRingShape`（外半径 + **壁厚** + 高度 + 位置/yaw）
→ `MakeRing16` → `BuildStripRing`。用壁厚而不是内半径作输入，是因为「玻璃壳的厚薄」才是
用户想调的参数，且天然保证 `0 < inner < outer`（越界 CHECK 崩）。
`test/strip/strip_geometry_test.cc` 的 `StripRing.*` 4 条用例把「四组点的高度/半径、
15 段闭合、哪组点当左轨/右轨（宽度方向是径向还是高度）」全部量化断言 —— 这几项接错时
画面**仍然是个圈**，只有量坐标才看得见。

### 19.2 菲涅耳参数（批次级）

接口：`StripFresnel`（`interface/strip_types.h`），`TranslucentStripBatch::fresnel`
是 `optional`：**不设 = 关闭**（与 `soft_depth_m` 同一写法，没有 `enabled` 字段）。

```
cosθ  = |N · V|                                   // 双面：法线朝里朝外都成立
F     = clamp((f0 + (1−f0)·(1−cosθ)^power) · strength, 0, 1)
color = mix(color, rim_color, F)                  // 掠过视线 → 反射色
α     = mix(α, graze_alpha · 逐点α, F)            // 掠过视线 → 更实（中心仍看得穿）
```

四个刻意的选择：

1. **用 `|N·V|` 而不是 `max(N·V, 0)`**：条带是单层片，没有「正面」概念，两面都该有边反。
2. **颜色和覆盖度一起动**：只调色 → 边反在浅色背景上看不见；只调 α → 边反变成灰边。
   两个一起混才是「边缘更亮且更像实体玻璃」。
3. **`graze_alpha` 不乘纹理/剖面 alpha，只乘逐点 alpha**：边反是**几何边**的属性，
   不该被贴图的透明区挖掉；但拖尾的逐点淡出要保留（否则尾巴又被边反拉实）。
4. **调用顺序固定**：`ModulateTexture` 之后、`WidthProfile`/裁形 AA 之前。剖面与 AA 仍是
   最后一层调制 —— 否则「柔边」开关会被边反覆盖，掠射处的抗锯齿也会被 α 抬高抵消。

`strength` 只是把 `F` 放大后 clamp，不是另加一项：这样 `strength=1` 就是物理量，
`>1` 是「比物理更像玻璃」的夸张，语义比「额外加一个高光项」更难被误解。

### 19.3 法线：两条 pass 各自的**精确**来源

§7 当初只写了导数法线（`cross(dFdx, dFdy)`），并预判「条带极细时可能闪」。这次按 pass 分开：

- **kPolyline**：仍然只能用导数法线（顶点里没有法线，VS 也不该为此多算）。它是**逐折面
  恒定**的 ⇒ 16 列圆环上一圈 15 个折面就是 15 条亮度带（实测 `160_fresnel_polyline_near`
  能看到竖条带）。
- **kSmooth**：曲面已经是 `P(t,w) = S(t) + w·H(t)`，法线直接是 `cross(dS, H)` —— **解析、
  无折面、零额外数据**（这两个量本来就在 FS 里算着）。实测 `170_fresnel_smooth_near`
  竖条带完全消失。这也顺手解决了 §7 担心的「细条带边反闪」：解析法线不依赖屏幕导数。

结论：**要看玻璃边反就用 kSmooth**；kPolyline 的边反是「能跑但不是最终观感」。
两条路都不是"平滑谎言"（顶点法线才是）：一条是精确的折面法线，一条是精确的曲面法线。

**契约（2026-10-22 Danis 定）：法线来源与风格绑定，没有独立开关。**

用户只有两种意图 —— 「柔和/条带」或「真折角」——`StripStyle` 就是这两个意图本身，
所以法线不该是第三个自由度。**「平滑几何 + 折面法线」这个混搭被明确毙掉**：
它只会产生与几何无关的亮带（15 段圆环那圈竖条），不对应任何使用意图，
却要多一个能配错的旋钮。`StripFresnel` 里因此**没有** `normal_mode` 之类的字段。

**算力开销（回答「柔和法线会不会更贵」）**

- **不会，反而更便宜。** 解析法线 = 一次叉积（6 mul + 3 sub）+ 一次 normalize
  （≈ 15~20 FLOP），而 `dS` / `Hh` **本来就在 FS 里算着**（Hermite 求值用的），
  纯寄存器复用：不增加顶点属性、不增加 varying、不增加 texelFetch、不增加带宽。
- 导数法线做的是**同样**一次叉积 + normalize，**外加** dFdx/dFdy 那套
  （硬件按 2×2 quad 求差分；软件光栅下是真的去取邻居样本）。
  所以同一份 fresnel 逻辑：解析法线 ≤ 导数法线。
- fresnel 本身（pow + 两次 mix + 一次 dot）约 30~40 FLOP，是「开不开边反」的固有成本，
  与法线来源无关。关闭时整段被 `uFresnelEnabled` 短路，画面逐字节等于旧版本。

### 19.4 实验预设与观感

`demo/strip_viewer_app.h` 的 `kFresnelPresets`（面板 Combo 与 `--capture` **共用同一份表**）：

| # | 预设 | f0 | power | strength | graze_alpha | 观感 |
|---|---|---|---|---|---|---|
| 0 | 物理玻璃 | 0.04 | 5 | 1.0 | 1.00 | 最像玻璃（标准 Schlick） |
| 1 | 柔边塑料 | 0.04 | 2 | 1.0 | 0.85 | 边很宽很软 ⇒ 亚克力/塑料 |
| 2 | 厚玻璃 | 0.10 | 4 | 1.4 | 1.00 | 边更亮更宽 ⇒ 厚壁玻璃，也很像 |
| 3 | 镜面边 | 0.16 | 6 | 2.2 | 1.00 | 过曝白边 ⇒ 像镀铬金属，**不是玻璃** |
| 4 | 窄边反 | 0.03 | 8 | 1.2 | 0.95 | 极细一条亮线 ⇒ 薄壁/灯泡玻璃 |

`--capture` 输出（都隐藏三条带、圆环居中、关掉参考方块）：
`100_ring_no_fresnel`（基线，只有淡蓝薄膜）、`110`~`150` 五个预设、
`160`/`170`（折线 vs 平滑，同一近机位）、`180`/`185`（默认场景 / 平滑 pass）。

### 19.5 viewer 与 gold 策略

- viewer 新增：中列「显示圆环 / 显示参考方块 / 圆环外半径 / 壁厚 / 高度 / 不透明度」，
  右列「菲涅耳边反开关 + 预设 + F0 / 掠射指数 / 强度 / 掠射不透明度 + 边反颜色」。
  面板两列 → **三列**（控件 30 个，两列会超出 720 高），状态行挪到窗口底部。
- 圆环**始终硬边（全 1 剖面）、始终纯色**：柔边剖面会把边反直接抹平，贴图会把边反盖成花纹。
  所以面板的「柔边剖面 / 使用纹理」只作用于三条带。
- **gold 不动**：`strip_gold.cc` 的 `ShotConfig` 显式写 `ring = false; fresnel = false;`。
  理由：gold 锁的是 M1~M3 的条带路径，把实验期的 fresnel 预设焊进逐字节 gold 只会让
  每调一次参数就重出一张图；圆环几何由单测锁，画面由 `--capture` 对照。
  唯一副作用：`strip_demo_1280x720.png` 有 **1 个像素差 1 LSB**（FS 多了 `vWorld` varying、
  指令重排），已重出；A/B 断言数值与之前完全一致（29419 / 2889 / 3540 / 3347 / 15980 / 5939 / 571）。

### 19.6 已知限制 / 未做

- 边反只作用于**覆盖集**的色与 α，没有独立的加法高光通道（§7 设想的「一条命令同时写两个
  集合」仍未做 —— 一个 batch 仍是一个集合）。想要「膜 + 加法高光」得发两个 batch。
- 反射色是常量 `rim_color`，不采样环境（没有 cube map）；亮边与背景无关。
- 圆环接缝（第 15 列 = 第 0 列）在 kSmooth 下两条端点切线不连续（各自单侧差分），
  接缝处参数化有一个很小的折点；折线 pass 天然没有这个问题。
- 圆环只有 15 段：折线 pass 的边反会有可见分面（这是 §19.3 的固有取舍）。

### 19.7 菲涅耳参数：per draw call 还是 per instance？（2026-10-22 讨论）

结论：**全部留在 per draw call（batch），不引入 per-instance。** 依据是现有不变式
「一个 batch = 一种材质 = 一次 draw call」：`texture_id / tint / blend / soft_depth_m /
width_profile / style` 已经在 batch 上，fresnel 跟它们是同一层的材质属性。

| 参数 | 归属 | 理由 |
|---|---|---|
| `power`（Schlick 指数） | **batch**（实际可视作渲染器常量） | 它是「用几阶近似」的模型参数，不是材质；一个场景里不该有两种 |
| `f0`（正入射反射率） | **batch** | 折射率 = 材质属性。换玻璃 = 换材质 = 换 batch |
| `rim_color` | **batch** | 反射/染色 = 材质属性（同 tint） |
| `strength` | **batch** | 美术夸张系数，跟着材质走 |
| `graze_alpha` | **batch**，但**已有 per-strip 通道** | 边缘实心度是「这层壳多实」。fresnel 里是 `mix(α, graze_alpha·逐点α, F)` ⇒ **缩放逐点 alpha 就能逐条带调边反浓度**（`Strip16::alpha` 本来就是 per-strip 数据） |

为什么不做 per-instance（以及将来真要做的话该放哪）：

1. **两条 pass 的代价不对称。** kSmooth 已经有「每条带一行」的数据纹理（每列 3 texel，
   12 float 用满），加一个 per-strip 值要么每列多 1 texel（+33% 该纹理带宽），
   要么另开一张 1 texel/条带 的小纹理（便宜，但要新增绑定 + 单元 + 生命周期）。
   kPolyline **没有任何 per-strip 通道**（顶点就是 6 float），加它要新增顶点属性 +
   每顶点打包 —— 那条路径本来是「最便宜」的卖点。
2. **一个 batch 已经是一种材质。** 要「同场景里不同玻璃」就发多个 batch（每个一次
   draw call）；几个玻璃物体多几次 draw call 在这个量级上不是问题。
   真正需要 per-instance 的场景是「同材质 × 上千实例」，而条带目前不做 instancing。
3. **per-strip 的 alpha 已经覆盖了唯一合理的 per-object 差异**（边反浓度）。

将来若真的需要在同一 batch 里区分玻璃种类（例如为了省 draw call），推荐做法：
**另开一张「1 texel/条带」的小纹理**（`kSmooth` 与 `kPolyline` 各加一个 strip id 属性 /
复用 `flat` 传入），而不是把 per-column 的数据纹理撑胖，也不要给 `Strip16` 加字段
（它是固定 POD，用户按 stride memcpy）。

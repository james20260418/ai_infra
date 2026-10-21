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
`fresnel_*`（M4）**等落地时再加**，不预留空字段。

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
| M4 | fresnel（导数法线） | ⬜ |
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

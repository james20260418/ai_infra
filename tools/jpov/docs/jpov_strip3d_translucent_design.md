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
// 2026-10-22 重构后的形态：纵向不透明度与颜色合并进逐点颜色（§19.11）
float a = point_color(u).a * tex.a * width_profile(v);
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
float a = point_color(u).a * tex.a * width_profile(v);   // §3.3 的最终 α（per-instance）
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

> ⚠️ **2026-10-22 重构后的接口才是当前形态**（下面这段是重构前的历史草图，
> 保留它是为了解释「为什么会有 batch tint / strip_tints / width_profile 三个来源」；
> 现行定义见 `interface/strip_types.h` 与 §19.11）：

```cpp
// 【历史·重构前】一条带：16 个采样点的双轨（POD，可整块 memcpy）
struct Strip16 {
    std::array<Vec3f, 16> left;    // 左轨（纹理 v = 1）
    std::array<Vec3f, 16> right;   // 右轨（纹理 v = 0）
    std::array<float, 16> alpha;   // 纵向 alpha 乘子 [0,1]
    uint32_t point_count = 0;      // 有效点数 [2,16]，必须显式给
};

// 【历史·重构前】一批条带
struct TranslucentStripBatch {
    uint32_t texture_id = 0;               // 0 = 纯色
    Color tint{1, 1, 1, 1};                // rgb 与纹理相乘；a 全局不透明度
    StripBlend blend = StripBlend::kCoverage;
    std::optional<float> soft_depth_m;
    std::array<float, 9> width_profile = {1,1,1,1,1,1,1,1,1};
    std::vector<Strip16> strips;
};

// 【现行】所有 per-instance 数据（几何 + 逐点 RGBA + 宽度剖面）都在 Strip16 里；
// batch 只剩材质与场景交接。详见 §19.11。
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
| 廉价玻璃 | `kCoverage` | 纹理（花纹）+ 逐点颜色的 `a` 控不透明度 + fresnel | 硬边用全 1 profile；观感=有色膜（§4.1） |
| 流星拖尾 | `kEmissive` | 逐条带宽度剖面 + 逐点 `a`（头 1 → 尾 0）+ 逐点 `rgb > 1`（HDR） | 宽度收窄是**几何**（左右点靠近） |
| 剑气拖尾 | `kEmissive` | 9-knot 单侧剖面 + 逐点纵向衰减 + 逐点 HDR 颜色 | 扇形是**几何**（扫角 + 半径） |
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

> ⚠️ 本节是 **2026-10-21 的实现记录**。2026-10-25 起（§19.12）：每列变成 **4 texel = 16 float**
> （第 4 个是逐点 rgba），每条带 768 → **1024 B**，FS 从 6 → **8 次 texelFetch**。

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

- **边反只作用于玻璃圆环**（Danis 2026-10-22 定）：库层面 `StripFresnel` 仍是批级参数，
  但 viewer 里只给圆环设 `batch.fresnel` —— 三条带/射流是发光拖尾，套边反会让边缘发白。
  同样，圆环的「是否平滑」也独立成一个开关（`ring_smooth_`），不和条带共用。
- viewer 新增：中列「显示圆环 / 圆环平滑 / 圆环外半径 / 壁厚 / 高度 / 不透明度」，
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

### 19.7 参数归属：per draw call 还是 per instance？（2026-10-22，**已被 §19.11 取代**）

> ⚠️ 下表的「颜色 / 不透明度 = per instance（走 `strip_tints` 平行数组）」「`width_profile` = per draw call」
> 是当时的结论；§19.11 的重构把两者都收进了 `Strip16`，并删掉了 `strip_tints` 与 batch tint。
> 保留原文是为了记录推导过程。

Danis 的判定：**「至少玻璃的颜色 / 条带的颜色，应该 per instance」** —— 是对的，
第一版把所有东西都留在 batch 上属于过粗。修正后的划分：

| 数据 | 归属 | 说明 |
|---|---|---|
| **颜色 / 不透明度（tint）** | **per instance** | 同一批里每条拖尾/每片玻璃偏什么色、多透，是实例属性。这是 `strip_tints` 通道的用途 |
| `f0`（正入射反射率） | per draw call | 折射率 = 材质。换玻璃 = 换材质 = 换 batch |
| `power`（Schlick 指数） | per draw call（实际可视作渲染器常量） | 「用几阶近似」是模型参数，一个场景不该有两种 |
| `rim_color`（边反颜色） | per draw call | 它是**反射到的环境色**的替身，不是物体色；同一环境里所有玻璃共用 |
| `strength` | per draw call | 美术夸张系数，跟着材质走 |
| `graze_alpha` | per draw call | 边「多实」是材质厚度感；逐实例的浓度已可被 `strip_tints[i].a` 整体缩放 |
| `width_profile` | per draw call | 剖面是材质（柔边/硬边），不是实例 |

**per-instance 是近乎零成本的**（这也是为什么值得做）：

- **拿条带下标不要钱**：折线 pass 的索引缓冲按「块 j = 顶点基址 j×32」静态生成，
  所以 VS 里 `gl_VertexID / 32` 就是条带下标（索引绘制里 `gl_VertexID` = 索引值本身）
  ⇒ **不新增任何顶点属性**；平滑 pass 本来就有 `aStripId`。
- 逐条带 tint 走一张 **RGBA32F、1 texel / 条带** 的小纹理（16 B/条带）+ 1 次 `texelFetch`。
  1024 条带 = 16 KB，纹理按需翻倍扩容。
- `Strip16`（固定 POD，用户按 stride memcpy）**一个字节都没动**：tint 是
  `TranslucentStripBatch::strip_tints` 这个**平行数组**（空 = 全用 `tint`，零开销）。
- 语义是**相乘**：`最终色 = tint × strip_tints[i]`。于是 `tint` 仍是「材质色 + 整批不透明度」，
  逐实例只做偏移/浓度。三条带 demo 也顺势把原来的「改写逐点 alpha」换成了这条通道
  （`ScaleRibbonAlpha` 删掉）：gold 的 7 个 A/B 数值与改前**逐位相同**，
  只有 2 个像素差 1 LSB —— 反向证明了乘法语义与旧实现等价。

不把 fresnel 的 `f0 / rim_color` 做成 per-instance 的理由：batch 已经是一种材质的边界
（`texture_id / blend / soft_depth_m / width_profile / style` 都在 batch 上）。真要在同一
batch 里区分玻璃种类，正确做法是**发多个 batch**；per-instance 的价值在「同材质 × 上千实例」，
而条带目前不做 instancing。

### 19.8 复核：`width_profile`（横向 9 个 knot）真的生效吗？

Danis 2026-10-22 提问。结论：**生效，且两条 pass 都是逐片元求值**（不是顶点插值，
所以 15 段的条带也能有平滑的宽度衰减）。证据：

- 代码路径：折线 `a *= WidthProfile(vUVAlpha.y)`、平滑 `a *= WidthProfile(vShade) * cov`
  （`v` = 宽度方向，0 = right 轨 / 1 = left 轨；平滑 pass 用的是**重建后**的 `v*`）。
- 定量：gold 的「柔边 vs 硬边」A/B = **15,980 px 差异**（区间断言 5,000~80,000）。
- 定向实验（临时把圆环剖面改成「只有 v=0.5 一个峰」/「只有 v=0 边」）：
  - 峰值剖面 ⇒ 只剩**沿条带长度的中缝横带**（立壁腰部一圈 + 顶面中间一个椭圆）；
  - 只留 v=0 边 ⇒ 只剩**立壁下沿 + 顶面内圈**（= `right` 轨 = `down_outer` / `up_inner`）。
  两种结果都是「沿宽度方向削」，不是「沿长度方向削」，且平滑 pass 与折线 pass 表现一致。

**顺带修掉一个真 bug（注释）**：`strip_types.h` 的 `width_profile` 注释还写着
「knots[k] = 在 **u** = k/8 处的乘子」—— 那是 §16.3 修正 UV 约定**之前**的写法
（当时 u 是宽度）。实际代码用的是 v（宽度）。已改正并注明历史原因：
这种过期注释会让人合理怀疑「横向调制到底有没有生效」，也就是这次提问的来源。

### 19.9 加法集 HDR 增益 + 逐点颜色 + 射流验收（2026-10-22）

补上 §19.6 里点名的两个缺口（Danis 指定）：

**① `TranslucentStripBatch::additive_gain`（optional，nullopt = 1.0）** —— ⚠️ 已于 §19.11 删除
（逐点颜色允许 HDR，增益就是颜色本身；新代码里没有这个字段）

- 为什么必须有：`tint` 被校验限制在 [0,1]、纹理也 ≤ 1 ⇒ 加法集贡献的上限就是 1.0，
  发光拖尾/射流永远吃不到 bloom 与 tone map 的 HDR 余量（tone map 之前是 scene-referred，
  亮部本该能到几倍）。
- 语义：`contrib = color · α · fade · gain`，只对 `kEmissive` 有意义 ——
  **给覆盖集设它会 CHECK 崩**（覆盖集没有加法附件，静默忽略等于骗人）。
- 校验：`(0, 1000]`；0 没有意义（想关闭请置 nullopt）。

**② `TranslucentStripBatch::strip_point_colors`（`vector<StripPointColors>`，空 = 不用）**

- `StripPointColors = std::array<Color, 16>`：**沿长度**的逐列颜色乘子，与
  `Strip16::alpha`（逐点淡出）互补 —— 一个管"什么颜色"，一个管"多透"。
- 实现：一张 **16 × 条带数** 的 RGBA32F 纹理，**双线性**过滤（这是唯一需要过滤的条带纹理）。
  采样坐标刻意落在 texel 正中心（`u = col/15 → tx = (col+0.5)/16`），列上是精确值、
  列间才插值；`ty = (vStripId+0.5)/rows` 保证不渗到相邻条带。
- **两条 pass 都是 1 次采样、零顶点属性**：折线 pass 的 `vStripId` 来自
  `gl_VertexID/32`（§19.7 已有），平滑 pass 用 `aStripId`；`u` 分别取插值 u 与重建的 `u*`。
- 为什么不进顶点属性：那要给折线 pass 加 3 float/顶点（6→9，+50% 带宽），
  而纹理一行才 16×16 B = 256 B/条带，且两条 pass 行为完全一致。

**③ viewer：火箭射流（`JetShape` / `BuildJetBatch`）**

一条射流同时验三件事：白热 → 橙 → 暗红的**逐点颜色**、尾部淡出的**逐点 alpha**、
中心实两侧虚的 **9-knot 剖面**，再叠 **加法集 + HDR 增益 + bloom**。
用**两条交叉的条带**（同一个 batch）：单条带是平面、侧看会消失，交叉后各角度都像个射流。

- 位置：喷口 (2.55, 2.35, 0.85)，方向 (0.18, −1, 0.10)，长 2.05 m，半宽 0.10 → 0.34。
- 面板：中列「显示射流 / 射流 HDR 增益(1~8，默认 3)」，左列「Bloom 辉光（HDR）」。
- `--capture` 新增：`200_jet_smooth` / `205_jet_polyline`（style A/B）、
  `210_jet_no_bloom`（辉光 A/B：实测 17,456 px 差异）、`215_jet_gain8`（增益 A/B）、
  `220_scene_with_jet`（默认场景）。折线/平滑在射流上只差 ~2,100 px（它是直筒状，差别在软边）。

**④ viewer 默认改成平滑 pass（`smooth_strips_ = true`）**

理由：圆环边反与射流软边只有 kSmooth 才是最终观感（§19.3）。gold **显式钉住折线**
（`app.smooth_strips_ = false`）并显式关掉 ring / fresnel / jet / bloom ——
gold 锁的始终是 M1~M3 那条三条带路径；本次改完两张 gold 与改前**逐字节一致**（0 px 差异）。

### 19.10 自 review 记录（2026-10-22，Danis 要求「注释写清楚 / 行为确定 / FS 看性能与安全」）

**性能（FS 与每批开销）**

| 问题 | 处理 |
|---|---|
| 关闭态的 uniform 每批重传（软深度 2 个、fresnel 6 个、加法增益、纹理行数…）| 改成 **Init 写一次**；每批只上传「开关 + 开启时的取值」。之前每批多 7~8 次 `glUniform`，1000 批 = 数千次驱动调用/帧 |
| 软深度淡出逐片元 `(scene_z−z)/m` 是除法 | 新增 `uSoftDepthInv = 1/m`，改成乘法（uniform 分母的倒数编译器不保证会提） |
| 逐点颜色用 `texture()`（隐式导数算 LOD）| 改 `textureLod(...,0)`：这张纹理没有 mipmap；平滑 pass 的 `u*` 在段边界被 clamp，导数本身脏 |
| `normalize()` 两次 sqrt | 改 `inversesqrt`（V 与 N 各一次） |

**还可以做、但这轮没做**（有意，避免为演示规模的东西加复杂度）：
- 每帧常量（near/far/viewport/camPos）用 dirty tracking 跳过重传 —— 注意 uniform 状态是
  **per-program** 的，缓存必须挂在每个 program 上，否则折线/平滑切换会漏传。
- Schlick 的 `pow` 在 `power == 5` 时用五次乘法特化（uniform 分支，多一条路径）。
  → **✅ 2026-10-25 已做（§19.12）**
- 逐点颜色纹理由 RGBA32F 降 RGBA16F（省一半带宽，10 bit 尾数对 [0,1] 颜色够用）。
- 平滑 pass 的逐点颜色其实可以并进「每列 3 texel」的数据纹理（多 1 texel/列 = +33%，
  但少一次采样 + 少一个纹理绑定）；这轮统一走独立纹理是为了两条 pass 行为完全一致。
  → **✅ 2026-10-25 已做（§19.12）**：每列改成 4 texel。

**安全**

| 风险 | 处理 |
|---|---|
| `texelFetch` 越界是 UB（不是返回 0）| 逐条带 tint 的下标按「本批实际条数」钳位（与 `uDataRows` 一致）|
| GLSL 里硬编码 32/16/15 与 C++ 常量漂移 | `Init` 里 `CHECK_EQ` 三个常量，忘了同步会直接崩，而不是静默错色 |
| `normalize(uCamPos − vWorld)` 在相机与片元重合时出 NaN | `d2 > 1e-12` 守卫 + 确定性回退向量 |
| `pow(1−d, k)` 在 d 冒到 1+eps 时出 NaN | `d` 先 `clamp(0,1)` |
| 新字段（`additive_gain` / `strip_tints` / `strip_point_colors` / fresnel）非法值 | 全在 `ValidateStripBatch` 拦（NaN 与越界都比较为 false ⇒ CHECK 崩）|

**行为文档**：`interface/strip_types.h` 里新增「一条条带的最终像素」小节，把颜色链 /
覆盖度链的每一步与顺序写死（含 fresnel 的 rim 取哪个 α、soft depth 与 additive_gain
乘在哪一步、两个集合各写哪套附件）。§7 那张「一条命令同时写两个集合」的草图仍然没实现 ——
现在同一批只能一个集合，要「膜 + 加法高光」还是两个 batch。

### 19.11 收口重构：per-instance 属性全进 `Strip16`（2026-10-22 Danis 定）

> ⚠️ 阅读提示：本节是 **2026-10-22 的收口记录**。其中「逐点 RGBA + 9 knot 剖面 → 一张
> per-instance 纹理（宽 25 texel）」对**折线 pass 与宽度剖面仍然成立**，但**平滑 pass 的逐点
> 颜色**已在 §19.12 改成从控制点数据纹理取（每列 4 texel），剖面仍旧走那张 per-instance 纹理。

**动机（Danis 原话的五个问题）**：同一份颜色有三个乘数（batch `tint` → `strip_tints[i]` →
`strip_point_colors[i][col]`）、逐条带配置文件与逐点颜色并行、`additive_gain` 与 HDR 颜色重复、
两个枚举注释太薄。结论是：**per-instance 的东西全部进 `Strip16`，batch 只留材质与场景交接。**

**现在的一条带（680 B，POD，用户按一个 stride memcpy / 存盘）**

```cpp
struct Strip16 {
    std::array<Vec3f, 16> left, right;    // 384 B  几何
    std::array<Color, 16> point_color;    // 256 B  rgb = 线性 HDR 颜色（可 > 1）, a = 纵向不透明度
    std::array<float, 9> width_profile;   //  36 B  宽度（v）方向剖面
    uint32_t point_count = 0;             //   4 B
};
```

- **颜色只剩一个来源**：`point_color`。「某条带整体偏色/整体更透」= 16 个同色（逐点颜色的退化特例），
  所以 `strip_tints` 与 batch tint 都删了。
- **逐点 alpha 并进 `point_color.a`**：它们本来就是两个相乘的标量。副产品是安全 ——
  「忘了填颜色」= 「忘了填 a」，`ValidateStripBatch` 的 `a > 0` 检查直接抓住
  （否则 `memset` 出来的条带会变成一条静默的黑色吸光带）。
- **宽度剖面 9 个数有默认值**（全 1 = 硬边，乘法单位元）：用户不填就是「不改宽度方向」。
- **没有 batch 级主不透明度**（Danis：一个 batch 里多条带的透明度很难统一，需要的人自己改逐点 a）。
- **`additive_gain` 删了**：加法集的注入量是 `color·α`，亮度就用 `point_color.rgb > 1` 表达
  （校验放宽到 `[0, 10000]`）。逐点 HDR 比一个整批倍率严格更强（芯亮/边暗可以自己画）。
- **`StripStyle` → `StripInterpolation`、字段 `style` → `geometry`**：名字直接说明它控制的是段间插值。

**GPU 侧（CPU stride 与显存布局解耦）**

| 数据 | 走哪条通道 | 每条带字节 |
|---|---|---|
| 几何 | 顶点属性：折线 5 float/顶点（pos + u,v）、平滑 6 float/顶点（+ strip_id） | 640 / 768 B |
| 逐点 RGBA + 9 knot 剖面 | 一张 per-instance 纹理：**一行 = 一条带**，行宽 25 texel（16 颜色 + 9 剖面），RGBA32F + LINEAR | 400 B |

- 采样坐标都落在 texel 正中心：颜色 `tx = (u*15+0.5)/25`、剖面 `tx = (16+v*8+0.5)/25`，
  行 `ty = (strip+0.5)/行数`（行数是纹理**容量**，不是本批条数）。
- **9 个等距 knot 的双线性 = 精确分段线性** ⇒ 剖面仍然逐片元求值（plateau 不会被抹成三角形），
  但只需要 1 次采样（重构前是 9 个 uniform + 一次 `glUniform1fv`/批）。
- 顶点属性少了一个 alpha（6→5 / 7→6 float），正好抵掉剖面 texel 的带宽：
  折线路径 1040 B/条带 → 1040 B/条带（320 B 省下 ↔ 128 B 增加，实测基本持平）。

**两条行为变化（有意，且都是「更一致」方向）**

1. **平滑 pass 的纵向 alpha 跟着重建后的 `u*` 走**（以前颜色用 `u*`、alpha 用顶点插值 `u`，
   两者在弯曲段会错位）。现在一次 `PointColor(u*)` 同时给颜色和 alpha。
2. **fresnel 不再乘「形状淡出」**：只有一个 alpha 通道后，「不透明度」与「沿长度淡出」不可分；
   若乘上去，α=0.12 的玻璃壳亮边也只剩 0.12（等于没有边反）—— 而反射本来就不该被介质透过率衰减。
   代价：开 fresnel 的条带别在长度方向做淡出，否则淡出段仍留边反（写进 `StripFresnel` 注释）。

**验证（gold 作为 no-op 护栏）**

新增 `test/strip:strip_scene_gold_test`：8 张固定构图的「全家福」（`strip_scene_*_1280x720.png`），
**在重构前用旧代码拍好并入库**，重构后逐像素比较。实测：

| 图 | 最大差 | 差 >1 的像素 | 说明 |
|---|---|---|---|
| 20 折线全家福 | 1 | 0（15 px 差 1） | 折线路径的 alpha 从顶点插值改为纹理双线性 ⇒ 纯浮点舍入 |
| 60 圆环 fresnel | **0** | 0 | 逐像素完全相同（边反/剖面/覆盖集都对上了） |
| 70 射流 HDR | 1 | 0 | 逐点颜色 + 剖面 + 加法集一致 |
| 10/30/40/50/80 平滑 | 2 | 18~59 | 上面第 1 条行为变化（`u*`）带来的亚像素差异 |

**这一轮没做、但记下来的**：加法集的早退门槛仍是按覆盖率 `a ≤ 1/255` 丢弃 ——
颜色允许 HDR 之后，「a 极低但颜色极亮」（注入量 > 1/255）的片元会被丢掉。改成按注入量
判据会改动画面（实测射流淡边多出 ~100~290 px、最大 9/255），需要重出 gold，留作单独一次。
→ **✅ 2026-10-25 已修（§19.12）**，那一次就是 §19.12。

两张老 gold（`strip_demo` / `strip_additive_soft`）分别差 5 / 6 个像素、最大 1/255。
因此 gold 比较从「逐字节」改成「逐像素 + ≤1（平滑图 ≤2 且 ≤200 px）的 LSB 容差」，
并在测试里写清原因 —— **任何可见变化都远超这个容差**。

### 19.12 收尾四项：加法集注入量门槛 + 两个热点特化 + 纹理 mipmap（2026-10-25）

Danis 2026-10-25 指定：「先做功能缺口的 ①③、性能余项的 ②④」——四条一起做，
能靠现有 gold 直接验证的就不用重出图。逐条记录（含**哪些是真的 no-op、哪些故意改画面**）。

**① 加法集早退门槛：按「注入量」判，不再按覆盖率**

`EmitFragment` 开头那道门槛从「`a ≤ 1/255` 就丢」改成按集合分流：

```glsl
if (uBlendPath == 1) {                       // 加法集：色可以 > 1（HDR）
    if (max(max(color.r, color.g), color.b) * a <= 1.0 / 255.0) discard;
} else if (a <= 1.0 / 255.0) {               // 覆盖集：aa = a·fade ≤ a ⇒ 判 a 等价
    discard;
}
```

- **覆盖集逐位不变**：`aa = a·fade`，`fade ≤ 1` ⇒ `a ≤ 1/255` 的片元下面无论如何都会被丢，
  所以还是判 α（同时保住「读场景深度之前就早退」这个最便宜的筛子）。
- **加法集真的变了**（这次改动的**唯一**可见效果）：射流增益 3 时颜色是 HDR，
  「α = 0.002 但色 = 4」的淡尾注入量 = 0.008 ≫ 1/255，旧代码在算 contrib **之前**就把它丢了。
- 实测（1280×720，同一机位）：**可见差异**（> 1/255）只落在射流/弧尾部，**+228~290 px、最大 8~9/255**，
  与 §19.11 当时预估的「100~290 px、最大 9」一致。（含射流的 6 张 gold 里另外还有几千 px
  是 ② 带来的 ≤1 LSB，两者要分开看：① 只在加法集、② 只在平滑 pass。）

**② 平滑 pass 的逐点颜色并入控制点数据纹理（每列 3 → 4 texel）**

- 布局：每列 `(c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)(rgba)` —— 第 4 个 texel 是逐点 RGBA。
- FS 端原本一段 `seg` 已经要取两列共 6 个 texel，现在取 8 个，然后用**段内真实参数**
  `tStar = clamp(t + dt, 0, 1)` 做 `mix(color0, color1, tStar)`。
- **为什么等价**：per-instance 纹理那张是 `GL_LINEAR`，在列间也是线性插值；`u* = (seg+t*)/15`
  映到 texel 中心恰好是「同权重的两点插值」。实测折线路径（20 号图）**完全没变**，
  平滑路径整片只有 **≤1 LSB**。
- 收益要说准（**修正 §19.10 的那句「少一次采样 + 少一个纹理绑定」**）：
  - **绑定并没有减少** —— `uAppearance` 还留着，因为宽度剖面（9 个 knot）仍走那张纹理
    （它的双线性 = 精确分段线性，挪进控制点纹理反而要手写 knot 插值，ALU 更多）。
  - 指令数也不是“白得”：**texelFetch 6 → 8 次**（+2），换掉的是 1 次跨纹理的 bilinear 采样。
    在真 GPU 上两者都是 1 条纹理指令 ⇒ **基本打平（甚至 +1）**；在 llvmpipe（本仓库所有
    gold / CI 跑的环境）上，双线性是软件展开的 4 个 texel + 权重 ⇒ 略赚。
  - **真正的收益是「颜色与 α 只有一个数据源」**：不再需要“两张纹理的采样坐标必须同步一致”
    这个隐含契约（§19.11 那一轮就因为它犯过 alpha/color 错位）。性能上别把它当优化宣传。
  - 代价：数据纹理 +33%（每条带 768 → 1024 B）。
  - ⚠️ 还有一个诚实的小代价：`cov` 裁形在 `discard` 之后才生效，而 8 个 texel 都在它之前取
    ⇒ **最终被裁掉的片元也多做了 2 次 fetch**（外扩最多 8× 时这类片元不少）。把 t3/t7 拆到
    discard 之后能省回来，但一个完整的列数据会被劈成“前 3 个 + 后 1 个”，与 CPU 打包顺序
    对不上、更容易改错 —— 相比 Hermite 那 ~140 ALU，这 2 次同行 fetch 不值得换可读性。
    **本轮未做单独 frame-time 基准**（gold 渲染里 PBR/CSM/bloom 占大头，测不出来不硬编数字）。
- **副作用（本轮故意不治）**：平滑批的 per-instance 纹理里那 16 列颜色现在**没人读**了，
  但 `UploadAppearance` 仍然整批上传它们（每条带 400 B）。要省得给「只看剖面的批」开一个
  变体路径（只传 9 个 knot），代价是两条 pass 不再是同一条上传路径 —— 而「两条 pass 共一份
  数据上传」正是重构时特地立的护栏，所以留着，等真到瓶颈再说。
- 副产品：颜色和 α 用**同一个** `tStar` 取（以前是颜色 `u*`、α 也 `u*`，但走的是纹理过滤），
  「颜色和 alpha 同源」这件事现在在代码层面是显然的，不再依赖「两处都记得改用 u*」。

**③ Schlick 的 `power == 5` 走五次乘法**

```glsl
if (uFresnelPow5 == 1) { falloff = edge*edge*edge*edge*edge; }
else                   { falloff = pow(edge, uFresnelPower); }
```

- `uFresnelPow5` 由 CPU 按 `power == 5.0f` **精确相等**给出（5 是 Schlick 默认值、也是所有预设的
  取值；近似相等只会让「走哪条路」取决于浮点偶然，更难排查）。
- 分支条件是 **uniform** ⇒ 一个 draw 内一致，没有 divergence。
- 两条路的浮点结果不逐位相同（驱动的 `pow` 是 `exp2(y·log2 x)`，精度本来就低于连乘），但实测
  **60 号图（圆环 + fresnel）重出前后逐字节相同**。

**④ 条带纹理的 mipmap —— 这不是库层缺口，是「默认值 + 缺开关」**

- `TextureOptions::mipmap` 早就有（`GL_LINEAR_MIPMAP_LINEAR` + `glGenerateMipmap`），
  `StripRenderer` 只是绑 texture id，什么都不用改。真正的现状是：viewer 里**故意关着**，
  因为素材 `ribbon_test.png` 是「横向 4 条色带」的手绘测试卡，两个方向都很缩时 mip 会把
  相邻色带糊成灰。
- 所以这次做的是：viewer 增加面板开关 `纹理 mipmap（远处防闪）`（同一张图以 mipmap=on
  另注册一份 ID，开关只换 id），并新增一对**远机位**拍摄 `300_far_tex_no_mip` / `305_far_tex_mip`
  （R = 14，只留三条带，关掉圆环/射流/方块/边反）。
- **两张图都不进 gold**：这是给人看的对比，不是回归门禁（gold 仍用 `ribbon_tex_`，
  也就是 mipmap=off，所以 8 张 gold 完全不受影响）。
- 实测（1280×720）：三条带合起来只占 **约 300×130 px**（差分区域实测 bbox x=513..814、y=289..422），
  此时关掉 mip 的纹理被打断成**断续虚线 + 闪烁感**（u 与 v 两个方向的缩小倍数都远大于 1），
  开了 mip 连续干净但色带明显变糊；两张最大差 **152/255**。
  结论：**测试卡留默认 off，真实素材该开**；这个开关就是给素材作者自己判断的。
- 三个已知代价（都不是 bug，但别当免费）：
  ① 常驻多一份纹理（~1.1 MB 含 mip 链）+ Setup 时一次 `glGenerateMipmap`（每张图一次）；
  ② 素材是**非预乘 alpha**（黑底转 alpha）：mip 会把透明 texel 的黑色 RGB 平进来
     ⇒ 半透明边缘偏暗。要干净得用预乘 alpha 或 alpha-coverage mip —— 属素材管线，不在 viewer 里做；
  ③ 没开各向异性过滤：掠射角下落 mip 会偏糊，而条带这种长条几何恰恰容易掠射
     —— 真要上生产得把 `TEXTURE_MAX_ANISOTROPY` 一起考虑。

**验证**

| 项 | 结果 |
|---|---|
| `bazel test //tools/jpov/test/strip:all`（顺序跑，`--local_test_jobs=1`） | **35/35 绿** |
| 60 号图（圆环 + fresnel + 平滑） | 重出前后**逐字节相同**（旧 gold 直接绿，未重出） |
| 80 号图（近距离全家福，平滑） | 旧 gold **仍然绿**（max=1，px>1=0，px>0=3482 ≤ 8000） |
| 10/20/30/40/50/70（含射流） | 因 ① 必然变 ⇒ 重出 gold（差异只落在射流尾部，见上） |
| `bazel build //tools/jpov:jpov_strip_viewer` + `--capture` 全表 | 通过；面板左列 14 行未挤出 720 高 |

**顺带记下的两个环境坑**

- `bazel run //tools/jpov/test/strip:strip_scene_gold_generator` **必须带 `-- --generate`**：
  不带参数时那份 binary 走的是「比对」分支，于是你会看到「重出 gold 却报 gold 不一致」。
- 三个 strip 测试**并行**跑会抢 Xvfb `:99`（`Server is already active for display 99` →
  `XIO: fatal IO error 22`）⇒ `strip_scene_gold_test` 会无故失败。本地/CI 用
  `--local_test_jobs=1` 即可复现为绿。

### 19.13 条带消费天光（over 型玻璃/薄膜）—— 两套 program + 与雾同构的注入（2026-10-25）

Danis 2026-10-25 定：「这个 over 型 strip 消费天光即可，此处和雾的公式应该是类似的：
① 有个吸收光的项转化为注入；② strip 本身也可以自发光。然后有一点特殊：我希望这个开启
光照消费的情况**不影响原本 pass 的效率**，这个开关控制。」

**模型（逐片元，接口权威见 `interface/strip_types.h:StripSkyLight`）**

```
C = point_color.rgb · tex.rgb                 // ② 自发光/表面色 —— 语义与不开光照时**完全相同**
  + injection · skylight(N)                   // ① 被吸收的背景光按天光补注入
skylight(N) = tricolor_lerp(N.y) · ambient_scale      // 上→天顶、水平→天际线、下→地
            + sun_color · sun_intensity · |N·L|       // 薄片双面：两侧都吃太阳
α = 覆盖率链（一点没改）
L_out = α·C + (1−α)·L_bg                      // WBO over 合成
```

- **与雾同构**：雾是 `L_scene·(1−α) + L_fog·α`，这里 `L_fog` 换成了「天光 + 自发光」。
- **注入量天然随不透明度缩放**（`C` 随后乘 α）⇒ 越厚/越不透明的材料补得越多，越透的补得越少，
  所以**没有**单独的「吸收系数」：有它会和 α 双重计数。`injection` 的物理含义是**散射率**
  （薄玻璃几个百分点，乳白/磨砂可以接近 1）。
- **不许用在加法集**：加法集本身即发光介质（注入量 = 色·α），再叠一套账会让 HDR 语义分裂
  ⇒ `ValidateStripBatch` 直接 CHECK 崩（不静默忽略）。
- 法线 N 与 fresnel **共用**：折线 pass 用屏幕导数（折面），平滑 pass 用解析法线 `dS × H`。
  太阳项取 `|N·L|`（单层双面片，与 fresnel 用 `|N·V|` 同一理由）。

**「不影响原本 pass 的效率」怎么做到的**

- 渲染器现在有 **4 个 program**：`strip_wbo_draw` / `strip_wbo_draw_lit` /
  `strip_wbo_smooth` / `strip_wbo_smooth_lit`。**主函数只有一份**，两套变体只差
  `ApplySkyLight()` 的定义：关态是**空函数**（编译器内联成空，指令数零变化），
  开态才是真注入 ⇒ 不会出现「两份 main 悄悄漂移」。
- 不用 uniform 分支：那会在每个片元上多一次分支 + 常驻寄存器，而这套东西只对少数批有意义。
- 证据：8 张 scene gold + 2 张老 gold **全部逐字节不变**（都走关态），40/40 单测绿。
  「指令数不变」是编译器内联空函数后的推断，**没有做 shader 反汇编核对**——
  严格成立的证据只有「画面逐字节相同」。
- 代价：多编译两个 program（Init 时一次）；开天光的批多 8 个 uniform（只在开时上传）。

**验收场景：橡树 + 开口玻璃缸（Danis 指定）**

- 树 = fire-fog viewer 同款 `models/tripo_oak_4k.glb`（归一化到 6m）。
- 缸 = `MakeStripRing` 的**底面 + 内外立壁**（顶面丢掉 ⇒ 开口缸，树冠从上面冒出来），
  当前尺寸：外半径 2.32m / 壁厚 0.04m / 高 4.32m / 中心高 2.16m（见 §19.15 的 -20%）。
  缸壁一条批（3 条带），平滑 pass + fresnel。
- 天光参数**与场景里的地面/方块同源**（同一份 `MakeSkyLighting` 的 `SkyCommand`）：
  `AmbientTricolor()` / `AmbientIntensity()` / `sun_dir` / `SunDirectionalColor()` /
  `SunDirectionalIntensity()`。
- 拍摄五张（**不入 gold**，它们要 4k 贴图 + 大模型，做门禁太重）：
  `400_tank_unlit`（关）、`405_tank_skylit`（开，散射率 0.20，太阳 34°）、
  `420_tank_skylit_inj40`（同开关 0.40 —— 用来定「玻璃 vs 乳白」该给多少）、
  `410_..._lowsun`（8°）、`415_..._night`（−8°）。
  实测整帧均值（**缸缩到 -20% 之后重测**，1280×720）：400 = (134,149,168) →
  405 = (143,156,176) → 420（散射率 0.40）= (147,160,179)；410（太阳 8°）= (124,127,126)；
  415（夜，−8°）= (46,46,51)（缸壁变暗而不是发光）。
  缸壁本身：顶部/朝阳侧更亮、底部/背阴侧更暗，且树始终看得见。
  CSM 的实际权重（临时把 `uHasCsm` 强置 0 重拍同一组）：405 差异 **1.96% 像素、最大 60/255**，
  410 **1.56%、最大 59/255**，形状是树枝/叶片的软块状剪影；400（不消费天光）**逐像素 0 差异**
  —— 反向证明「不开天光就完全不采 shadow map」。
- **坑**：glb 必须在**首帧之前**加载（`PrepareTankScene()`）。在 `OneIteration` 里懒加载会撞
  `GL error after mesh upload, code=1282` —— 那时绑定状态属于上一帧的 pass。
  也刻意**不在 `Setup` 里自动加载**：gold 的 8 张图不用这个场景，不该替它付 4k glb 的代价。

**已知限制（都是有意的）**

- ~~不采阴影~~（**已由 §19.15 补上：太阳项乘 CSM，天光项不乘**）。条带**自身**仍不投影
  （不写 shadow map，需要给半透明片单开一次 shadow pass）。
- 天光是**方向无关的近似**（三色 × 法线 y + 太阳项），不是 IBL：没有环境遮蔽、没有
  方向性漫反射积分，也没有反射方向采样（那属于功能缺口 ④-2，需要 env 图或解析天空）。
- 缸壁在夜里会显得「偏实」：夜色 ambient 很小但非零，而 α 不变 ⇒ 反差变小，属预期。

### 19.14 viewer 场景化 + 左下角面板（2026-10-25）

Danis：「现在的 viewer 有点不好操作了，仿照 particle viewer 那样，用下拉菜单控制数个场景：
① box + 旋转三条带；② 射流单独一个场景；③ 玻璃缸 + 树；原来的玻璃缸不要了。注意每个场景下
参数选项要根据场景是否用到动态改变 UI，UI 整体放左下角，黑色半透明底板装一下。」

**三个场景（`StripViewerApp::Scene`）**

| 场景 | 内容 | 参数（面板只列这些） |
|---|---|---|
| ① `kRibbons` | 地面 + 四块参考方块 + 三条带 | 使用纹理 / mipmap / 不透明度 / 条带宽度 / 柔边剖面 / 条带平滑 / 弧形用加法集 / 软深度(+距离) / 波带振幅 / 螺旋转数 / 弧形开角 / 弧形朝向 / 显示波带·螺旋·弧形 / 形状动画 / 自动绕行 / 显示参考方块 / SMAA |
| ② `kJet` | 射流（加法集 + HDR + bloom） | 射流 HDR 增益 / 条带平滑 / Bloom / SMAA |
| ③ `kTank` | 橡树 + 开口玻璃缸（sky_light） | 缸壁消费天光 / 天光注入比例 / 缸壁不透明度 / 缸体半径 / 缸体高度 / SMAA |

- **场景只是「一组可见性预设」**：`SelectScene()` 写一次开关（顺带给该场景一个合适机位），
  之后各开关仍可单独调。**gold 与 `--capture` 直接设开关、不经过场景** ⇒ 8 张 scene gold
  与这轮 UI 改动完全无关（41/41 仍然绿，逐字节不变）。
- **动态 UI 有两层**：① 只列当前场景用得到的参数；② 按值裁剪（关纹理 → 不显示 mipmap；
  关软深度 → 不显示距离）。行数 > 14 时同一条底板内自动分两列（保证 720 高放得下）。
- **底板**：`cmds->DrawFillRect(...)`，黑色 alpha 0.55 + 1px 白 10% 边框 + 10px 圆角，
  左下角锚定（16px 边距）。必须先于 `ui_.Emit(cmds)` 进命令表才会画在控件下面
  （2D 指令按 `cmds.order` 顺序绘制）。底部一行提示「右键拖动=绕行 滚轮=推拉」+「重置」。
- **旧玻璃圆环（ring）**：从所有场景移除，也不再进面板；但**代码与字段保留** ——
  `strip_scene_gold.cc` 仍用它锁 fresnel（预设 2=厚玻璃）、环几何、折线/平滑两条 pass。
  拍摄表里原来 100~185（圆环 + 五个边反预设）随之删除，边反 A/B 改由缸壁承担（430/435）。

**拍摄表变化**：`10~80`（场景①）、`200~215`（场景②）、`400~435`（场景③，含缸壁边反 A/B）、
`300/305`（远机位 mip A/B）、`98_panel_0/1/2`（三个场景各一张，证明参数随场景变）。
一共 27 张。

**改造时踩到的两个真 bug（都已修，值得记一下）**

1. **射流被「三条带 A/B」开关管着**：`EmitJetBatch` 原来写在 `EmitStripBatches()` 里，而那个
   函数只在 `show_strips_` 为真时调用。场景② 把 `show_strips_` 置 false（射流场景不该有
   三条带）⇒ 射流一条都画不出来，拍出来只剩地面。修法：射流提到 `OneIteration` 里独立发，
   谁都不再隐式管着它。
2. **拍摄表的「缺省 true」会覆盖场景预设**：第一版 `Shot` 用带默认值的 bool（`show_strips=true`、
   `ribbons=true`…），循环里无条件写回 ⇒ 场景② 里三条带和方块又冒出来。改成
   **全部可选覆盖**（`std::optional`，不写 = 用场景预设），并在注释里写明原因。
3. **太阳仰角跨张残留**：`SelectScene()` 只重置场景自己的参数，天光（仰角/方位/浑浊度）是
   三个场景共享的面板参数，没人重置 ⇒ `415_..._night`（−8°）之后拍的 `420`（散射率 0.40）、
   `430/435`（缸壁边反 A/B）**全都是夜景**，均值从 (147,160,179) 掉到 (48,48,52)，
   看上去像「乳白缸变黑了」。修法：`SelectScene()` 连天光一起回默认（切场景 = 干净起点）。

### 19.15 条带消费 CSM 阴影（2026-10-25，与天光同一个开关）

Danis 提问 + 定调：「这个光照的 CSM 消费了吗？…我理解还是要的，而且条带本身或许应该也参与 CSM
的生成？但这个做起来太笨重…先不搞。本次要求 CSM 被 over 类型 strip 消费，开关和天光消费一起，
这俩绑定。」

**之前的状态（回答那个提问）**：§19.13 的天光消费**只吃 CSM 之外的项** —— 三色天光 + 太阳
`|N·L|`，完全没有采样 shadow map。所以玻璃缸在树的影子里也照样亮，文档里也把「不采阴影」
列成已知限制。

**现在的做法**

```
skylight(N) = tricolor_lerp(N.y)·ambient_scale            // 环境光，**不乘**阴影
            + sun_color·sun_intensity·|N·L|·CSM(worldPos, dist)   // 直射光，乘阴影
```
- **没有独立的阴影开关**：`sky_light` 有值就采样 CSM（开关绑定，Danis 要求）。
- 复用主链路**同一套资源**：`StripRenderer::Draw` 新增 `const StripCsmShadow* csm`
  （由 Renderer 在按下条带 pass 时填：`cmds.sun` + `shadow_fbos_` + 各级联 VP/深度 VP/单纹素边长
  + `ShadowConfig`）。shadow map 绑在 `kTexUnitShadowMapBase`（7..11）——与主链路同段，
  因为本 pass 与 object3d 不同时活跃（和 tile/材质同一约定）。
- **单级联单次采样、无 PCF**，按片元到相机距离选级联，末尾按 `fade_start/end` 淡出 ——
  与 fire_fog 的雾体阴影**同一套写法与理由**：条带是薄片、片元多，PCF 的 9~64 次采样换不来
  可见收益；而且 shadow map 是按**不透明几何**生成的，对半透明片做精细 PCF 没有物理意义。
  因此阴影边缘是「软块状」而非清晰（这是**取舍**，不是 bug）。
- 深度偏置用 **tanθ=1 的保守值**（`max(0.01, 1.5·texelW)`），理由同雾：薄片法线与阴影深度轴
  经常近乎正交，按 N·L 推导会低估偏置 → 自阴影 acne。
- 没有太阳 / 没有级联 ⇒ `uHasCsm = 0`，太阳项退化为完全受照（不崩、不改颜色语义）。

**成本**：开天光的批每 draw 多 5 张纹理绑定 + ~12 个 uniform；每片元多 1 次纹理采样
（`CsmTap`）+ 2 次 mat4×vec4（`uShadowVP` / `uShadowDepthVP`）+ 一次级联选择循环。
`sun_intensity == 0`（纯天光 / 入夜）时整段太阳项跳过 ⇒ 没有额外采样。
**不开天光的批仍然是原来那两个 program** —— 10 张 gold 逐字节不变、41/41 单测绿。
实现上 CSM 采样只有一份（`src/csm_shadow_glsl.h`，与 fire_fog 共用），避免两处漂移。

**验收（缸景）**：405/410 里树在缸壁内侧投出**可见的软影**，朝阳侧亮、背阴侧暗；
400（不开天光）没有任何阴影。缸的默认尺寸同时按 Danis 要求**整体缩小 20%**
（半径 2.90→2.32、高 5.40→4.32、壁厚 0.05→0.04、中心高 2.70→2.16，比例不变、底面仍在 y=0）。

**仍未做（Danis 明确先不搞）**：条带**自身**参与 CSM 生成（写 shadow map）。要做都得给半透明
片单开一次 shadow pass（或用 alpha-tested 变体），笨重且和「条带是 effect pass」的定位冲突。

### 20. 采样数多态化：4 / 8 / 16 / 32 四档并列（2026-10-26）

需求（Danis）：「16 这个采样数的多态化，draw call 命令上直接体现在接口上（就是
`DrawStrip16Batch` 这种），上次模板类的实现给搞崩了……这次只做 Strip4，和 Strip16 并列。
验收用 strip viewer 的新场景做采样对比，Strip4 三个实例、Strip16 也三个实例，纵向错开。」
随后追加：「补齐 8、32 这两个实现；上下错开不是我的本意，我本意是**沿 Z 轴**
（y-up 里的 z）错开，**四种配置同屏、精度逐渐变高**，再加个滑条让整体沿 Z 平移 ±5m 便于切换观察。」

**四档 = 四种拓扑，不是四个精度档**

| 档位 | 段数 | 顶点/条 | 索引/条 | 行宽 texel | 典型用途 |
|---|---|---|---|---|---|
| 4  | 3  | 8  | 18  | 13 | 雨丝、辅助线、道路标线（数量多、单条短、本来就直） |
| 8  | 7  | 16 | 42  | 17 | 中短拖尾、光束、简单弧线 |
| 16 | 15 | 32 | 90  | 25 | 默认主力：普通拖尾、波带、闭合圆环 |
| 32 | 31 | 64 | 186 | 41 | 长而弯的东西（大回环、飘带、需要极致顺滑的边缘） |

**接口层：四条并列命令、四个类型（不是模板）**

| | 4 | 8 | 16 | 32 |
|---|---|---|---|---|
| 条带类型 | `Strip4`（200 B） | `Strip8`（296 B） | `Strip16`（680 B） | `Strip32`（1304 B） |
| 批类型 | `TranslucentStrip4Batch` | `TranslucentStrip8Batch` | `TranslucentStrip16Batch` | `TranslucentStrip32Batch` |
| 命令 | `DrawStrip4Batch` | `DrawStrip8Batch` | `DrawStrip16Batch` | `DrawStrip32Batch` |

- 为什么不用模板：模板参数会一路渗透到渲染器、数据纹理、索引缓冲、后处理，
  任何一处实例化不对都是链接期/运行期的灾难（上一版就是这么崩的）。这里改成
  **运行期多态**：`StripSamplesView`（左/右轨 + 逐点颜色 + 剖面 + 点数 + 容量）+ `StripLayout`
  （采样数 / 段数 / 顶点数 / 索引数 / 行宽），两者都是普通 POD。
- 批级材质抽成 `StripBatchStyle` 并让四个批类型继承它：`batch.blend` / `batch.sky_light`
  这些字段在四档上写法完全一样，校验也只有一份（`ValidateStripStyle` + `ValidateStripSamples`）。
- `StripLayoutSlotOf(samples)` 把采样数映射到资源槽位（0..3）；非法采样数返回 -1，
  调用方 CHECK 崩 —— 不静默画错档位。

**实现：一套 CPU 数学 + 一份 GLSL**

- CPU（`strip_geometry.h`）：`ViewOf(Strip4|8|16|32)` → `StripSamplesView`；
  `ExpandStripVertices(view, layout, …)` / `ComputeSmoothStrip(view, layout, …)` /
  `AppendSmoothStripData(view, layout, …, row_stride_floats, …)` /
  `AppendSmoothStripVertices(view, layout, …)`。四档老签名都保留薄重载（调用点不用改）。
- 平滑暂存 `SmoothStripCompute` 按**最大档**（32 列）分配，四档共用：只有前 `layout.samples` 项有效。
- GLSL **只有一份**（没有为每档编译一套 program）：原先写死的 15/25/16/32 换成
  4 个 uniform（`uPointColorScale/Bias`、`uProfileScale/Bias`）与 `uVerticesPerStrip`，
  值由 `StripLayout` 现算 —— 16 点档与旧常量逐位等价（gold 逐像素不变，≤1/255）。
- 资源：per-instance 纹理按档位各一张（行宽 13/17/25/41，列坐标必须按本档行宽归一化）、
  索引缓冲各一条（18/42/90/186 索引，顶点基址 = 2×采样数），VAO/VBO 共用；
  平滑 pass 的控制点数据纹理四档共用一张（行跨距按最大档 512 float，小档位行尾补 0）。

**踩到的真坑：索引类数值绝不能走浮点**

`vStripId`（= 本条带的 per-instance 数据行号）最初写成 `float(gl_VertexID) * (1/32)`。
在 llvmpipe 上浮点除法会给出「**略大于整数**」的结果（如 1.0000001）⇒ `AppearanceRow()`
落在两行之间 ⇒ 硬件双线性把**相邻两条带**的颜色与剖面混起来：画面表现是「颜色变淡发灰、
条纹变糊」，既不崩也不报错，gold 直接差 134/255。修法：行号一路用 **int**
（VS 里 `gl_VertexID / uVerticesPerStrip` 整数除法 + `flat out int`；平滑 pass 的
`aStripId` 属性同样 `int(aStripId + 0.5)`）。

**验收场景（viewer 场景④「采样对比」）**

- 四条**同样的曲线**（波 / 螺旋 / 弧）、同一份材质（纹理 + 柔边剖面 + 同样的逐条带浓度），
  只换采样密度：4 / 8 / 16 / 32 各一批（一批 3 条、各一次 draw call）。
- 排布沿 **Z 轴**（y-up 里的深度轴）等距排开、**精度递增**（4 最近 → 32 最远，
  间距 `sampling_row_gap` 面板可调）；面板另有 **「整体 Z 平移 m」滑条（−5…+5）**，
  把四排一起挪，方便在同一机位下凑近看某一档。
- 为什么每档都是 3 条：draw call 数只与**批数**有关、与条带数无关，一批多条才看得出这一点；
  而且每条带在四档里各有一个对照，避免把「单条带的差异」误当成材质差异。
- 拍摄：`500_sampling_rows`（全景）/ `505_sampling_zoom`（近机位）/ `510_sampling_smooth`
  （四档都走 Hermite）/ `515_sampling_zshift`（整体 Z +2.5m，证明滑条真的在动）。
  四张都**不进 gold**（图形对比是给人看的，不是回归门禁）。
- A/B 证明「每一排确实来自它自己的命令」：临时去掉 `DrawStrip4Batch` 重拍，
  只有 4 点档那一排区域的像素变化。

**性能口径（不要夸大）**

- 顶点/索引随档位线性变化（一条带 8/16/32/64 顶点、18/42/90/186 索引）。
- **片元成本四档相同**：都走同一份 FS，per-fragment 一样 ⇒ 4 点档**不会**减少片元数
  （覆盖的像素一样多）。省的是顶点数据带宽与顶点着色，价值在**顶点瓶颈**场景
  （数量多、单条短：雨丝、辅助线、道路标线），不是「画得更快」。
  反过来说，32 点档也不便宜：一条带 64 顶点，几千条时顶点/索引量是 4 点档的 8 倍。
- 没做 frame-time 基准 —— 上面是结构上的必然，不编数字。

### 21. 退化输入（几何数值退化）—— 会不会除零 / 出 NaN

Danis 的问题：「万一 JPOV 用户输入了奇异的数值，JPOV 该不会崩溃吧？NaN 和 Inf 我们可以约定
用户不会犯傻（render command proto 化时能证据实锤），但**像重叠点这类输入**，会不会让
renderer 或者内部除以 0？」→ 本节是对**整条 strip 链路**的一遍清扫结果。

**先说结论**

1. **能"合法但退化"的几何（重叠点 / 零宽 / 亚微米段 / 折返 / 沙漏 / 远坐标）不会崩，
   也不会产生 NaN。** 结构上的原因很硬：这些退化在屏幕上都是**零面积**（四边形的两条边
   重合成一条线）⇒ 光栅化阶段就没有片元 ⇒ FS 里那些除法根本不会被执行。
   GPU 端探针实测：把九种退化条带画进画面，**零面积的三类改变 0 个像素**。
2. 能光栅化出来的退化（沙漏、楔形尖端、折返）确实会走到 FS 的除法，但**每个除法后面都
   跟着守卫或 clamp**（见下表），所以结果是有限值 —— 要么被裁掉（落到条带轮廓之外），
   要么画出一条极细的正常条带。
3. **真正找到并修掉的除零点在"相机"上，不在几何上**：`ZView()` 的
   `2·near·far / ((far+near) − ndc·(far−near))`，当用户给 `near = 0` 或 `near ≈ far` 时
   分母会趋近 0（恰在远平面时是 0/0 = NaN）。NaN 一旦写进 1× WBO 就会顺着 composite
   铺满全屏 —— 这是"一层膜把整张画面拖黑"的典型事故。已在 strip / particles / volume_fire
   三处**同一段函数**上加了守卫（正常相机下分支永不进入 ⇒ 三者的 gold 全部逐像素不变）。

**逐点清单（strip 链路里所有除法/归一化）**

| 位置 | 表达式 | 退化时靠什么扛住 |
|---|---|---|
| `strip_geometry.h` 列 u | `col / layout.quads` | 布局由断言保证 `quads = samples−1 ≥ 3` |
| `ComputeSmoothStrip` 外扩 | `margin / min_half` | `min_half` 只在 `|h| > 1e-5` 的列里取；全零宽时保持 1e30 ⇒ 回落 `scale = 1` |
| 同上（单位外法向） | `lh * (1/len)` | `len < 1e-5 ⇒ continue`（该段没有"外侧"） |
| 平滑 FS 段内参数 | `(D·dS) / max(dS·dS, 1e-8)` | 守卫 + 后面 `tStar = clamp(...)` |
| 平滑 FS 宽度参数 | `(D·Pv) / max(Pv·Pv, 1e-8)` | 守卫（Pv = 2·Hh，零宽列会让它 → 0） |
| 平滑 FS 段号 | `(seg + tStar) / uSegCount` | `uSegCount = quads ≥ 3`（uniform 由 layout 给） |
| 平滑 FS 裁形 AA | `min(v*, 1−v*) / fwidth(v*)` | `fwidth` 有 `max(..., 1e-5)` 地板 |
| 折线 VS 行号 | `gl_VertexID / uVerticesPerStrip` | **整数除法**（浮点会给"略大于整数"，见 §20）+ CPU 侧 CHECK > 0 |
| per-instance 行号 | `(row + 0.5) / rows` | `rows = max(int(uAppearanceRows), 1)` |
| 菲涅耳 | `inversesqrt(d2)` / `inversesqrt(n2)` | `d2 > 1e-12` / `n2 > 1e-12` 分支，否则退化到固定值 |
| 天光 | `inversesqrt(n2)` | 同上 |
| 深度线性化 | `2·near·far / ((far+near) − ndc·(far−near))` | **本次新增守卫**：`!(|denom| > 1e-6)` ⇒ 返回 `max(near, 1e-4)` |
| WBO 解析 | `accum.rgb / accum.a` | `accum.a ≤ 1e-7` ⇒ 直接输出 0（原有守卫） |

**测试（都是可证伪的，不是恒真断言）**

- `strip_degenerate_test`（新，GPU 端，headless）：同机位渲四张图 —— 对照（不发条带）/
  零面积退化 / 有面积退化 / 有面积退化 + 一条正常条带。断言：
  ① 零面积那批**改变 0 个像素**；② 有面积那批**条带区域之外逐像素相同**（这是 NaN 的
  探测器：NaN 泄漏会铺满全屏）；③ 同批的正常条带仍然可见（差异像素数严格变大且 > 500）。
- `strip_geometry_test` 的 `StripDegenerate.*`（CPU 端，GL-free）：九种退化配方 + "两点重合"
  + "零宽回落 scale=1" + "圆环壁厚 1e-7"，判据统一是**每个输出都是有限值**、外扩倍数落在
  `[1, 8]`。

**已知不做 / 边界**

- NaN/Inf 输入不做处理：按约定由 render command proto 化时拦掉（本层只保证"有限值进去 ⇒
  有限值出来"）。
- 坐标量级失控（例如 1e30 米）不会被拒绝；外扩量会被 `kSmoothMaxWidenScale = 8` 截断，
  极端细尖仍可能缺一条亚像素（既有已知限制，与本轮无关）。
- 退化几何**不报错**是刻意的：零宽 = 一条线、重合点 = 零长度段，都是合法表达手段，
  在 `ValidateStripBatch` 里拒绝它们等于把用户的合法几何判成非法。

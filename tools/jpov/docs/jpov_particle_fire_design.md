# JPOV 主角「燃料火」— 广告牌粒子（Billboard Particle）火特效设计

> 日期：2026-10-14 ｜ 状态：**设计（待实现 MVP）** ｜ 目标工作区：`/james_pm/ai_infra_4`
> 方向：**偷师 UE / Unity 的成熟火特效做法** —— 「广告牌粒子 + flipbook 序列帧 + 生命周期曲线 + 多发射器分层」。
> 关联：`tools/jpov/docs/jpov_fog_design.md`（环境雾火 froxel，已实现）；`/james_pm/fire_hero/`（资产 + Mantaflow 烘焙管线 + 逐像素 raymarch 设计）。
> 上一版方向（逐像素体素 raymarch）见 `/james_pm/fire_hero/DESIGN.md` —— **本轮改为广告牌粒子**，raymarch 降级为「可选增强」保留。

---

## 0. 一句话 & 本轮的方向变更

**做法**：一个主角火 = **一组发射器（emitter）**，每个发射器持续吐出**朝相机的广告牌小面片**，贴 **flipbook 序列帧（一排/一片火帧）**，用 **生命周期曲线**（尺寸/颜色/速度随寿命变化）+ **加法/透明混合**组合出火焰的体积与动态。

**为什么改**：
- **这是 AAA 的真实做法**。UE（Cascade / Niagara）与 Unity（Shuriken / VFX Graph）里，绝大多数「主角火」都是广告牌粒子 + flipbook，**不是实时体渲染**（体渲染留给离线或极少量 hero 镜头）。
- **便宜、可控、稳定**：几百~几千个粒子就够了；效果可逐参数调；无 froxel 的 tile 伪影/断裂问题。
- **不依赖 GPU 特性**：纯 instanced quad + 纹理，llvmpipe 软件光栅也能预览（本容器日常环境）。
- 体渲染（raymarch，`fire_hero/DESIGN.md`）**不是不能做**，但它成本高、需要真 GPU、且是「另一条技术栈」；先上业界验证过的最优性价比方案。

---

## 1. 为什么能「偷师」：火的通用设计骨架

跨引擎共通的关键元素（这就是要偷的东西）：

| # | 元素 | 作用 | UE 叫法 | Unity 叫法 |
|---|------|------|---------|-----------|
| 1 | **广告牌朝向 (Billboarding)** | 面片始终/部分朝向相机 | Sprite Renderer Alignment/Facing | Render Alignment（View/World/Facing/Velocity） |
| 2 | **Flipbook / SubUV** | 序列帧动画（火的形状演变） | SubUV（SubImage Index / SubUV Movie） | Texture Sheet Animation |
| 3 | **生命周期曲线** | 尺寸/颜色/速度随寿命变化 | Distributions（Curve）over Relative Time | * over Lifetime 模块 |
| 4 | **多发射器分层** | 核心焰+主焰+烟+火星+光 | Emitter（栈） | Sub Emitters / 多个 ParticleSystem |
| 5 | **发射形状** | 从锥/圆/盒/网面喷出 | Location 模块 | Shape 模块 |
| 6 | **混合模式** | 加法发光 vs 半透明遮烟 | Blend Mode（Additive/Translucent） | Material blend（Additive/Alpha） |
| 7 | **排序** | 半透明背→前；加法无所谓 | Sort Mode | Sort Mode（None/By Distance/…） |
| 8 | **光照** | unlit 为主；可选打光 | Material Unlit / Particle Light | Renderer/Shuriken Lights 模块 |

**核心认知**：*火不是一个 emitter，是一组 emitter*。好看的燃料火 = 各司其职的层叠：
`核心亮焰` + `主火舌` + `2~3 层烟` + `火星/余烬` + `热扰动` + `点光`。
形状/细节主要来自 **flipbook 贴图**，动态主要来自 **粒子运动 + 生命周期曲线**，观感主要来自 **加法混合的亮度叠加**。

---

## 2. UE 篇（Cascade + Niagara）

### 2.1 发射器栈模型

**Cascade（UE3/4 经典）**：
- 一个 **ParticleSystem** 含若干 **Emitter**（发射器）；每个 Emitter = **一摞模块（modules）**，按类别分组：
  `Acceleration / Attraction / Collision / Color / Event / Kill / Location / Material / Orbit / Rotation / Size / Spawn / SubUV / Velocity`。
- **TypeData** 决定发射器类型：**Sprite**（默认，2 多边形卡片，永远面向相机 —— 火/烟/火花的主力）、Mesh、Beam、Ribbon。
- 参数用 **Distributions** 表达：`Constant`（常数）、`Uniform`（区间随机）、`Curve`（随寿命/时间变化的曲线）、`Constant Curve` 等。**这是「曲线」的来源**。
- 支持 GPU Sprites（大量粒子）、LOD、Vector Fields（矢量场驱动）、Particle Lights。

**Niagara（UE5，现代）**：
- **System → 多个 Emitter**；每个 Emitter 内是**栈（stack）**，模块按执行时机分组、**自上而下顺序执行**：
  1. `Emitter Spawn`（CPU，建发射器时一次）
  2. `Emitter Update`（CPU，每帧）
  3. `Particle Spawn`（每粒子出生时一次）
  4. `Particle Update`（每粒子每帧）
  5. `Render`（渲染器：Sprite / Mesh / Ribbon / Light）
- 数据模型：参数分 **Primitive / Enum / Struct / Data Interface**；User Parameters 可从外部（Blueprint/C++）注入。
- Execution State：`Active / Inactive / InactiveClear / Complete`。

### 2.2 火常用模块 + 关键参数

| 模块（Niagara） | 关键参数 | 火里怎么用 |
|---|---|---|
| **Spawn Rate** | Spawn Rate / Burst | 每秒吐多少粒子（火焰约 20~100/s） |
| **Initialize Particle** | Lifetime、Position、Mass、Color、**Sprite Size**、**Sprite Rotation**、Mesh Scale | 出生时的寿命/位置/颜色/尺寸/朝向 |
| **Add Velocity** | Velocity、Velocity Scale、Coordinate Space | 初速度（向上 + 锥形扩散） |
| **Gravity Force** | Gravity | 重力（火常给**负重力**/浮力向上） |
| **Drag** | Drag、Rotation Drag | 阻尼（让粒子减速） |
| **Scale Color** | Scale RGB / Scale Alpha | 按寿命缩放颜色（做颜色带） |
| **Scale Sprite Size** | Scale / Non-Uniform Scale | 按寿命缩放尺寸 |
| **Curl Noise Force** | Noise Amount / Frequency | **湍流噪声**（火苗抖动，关键！） |
| **Solve Forces and Velocity** | — | 积分速度/位置（GPU 模拟必需） |
| **Sprite Renderer** | 见 §2.4 | 出片 |

### 2.3 SubUV / Flipbook（**最核心**，火形状的来源）

**Cascade 的 SubUV 模块**：
- `InterpolationMethod`（在 TypeData 上）：`PSUVIM_None`（关）、`PSUVIM_Linear`（整帧切换）、`PSUVIM_Linear_Blend`（**相邻两帧混合**，最平滑）、`PSUVIM_Random`、`PSUVIM_Random_Blend`。
- **SubImage Index** 模块：一个 float distribution，**按粒子的 RelativeTime** 索引子图。子图顺序 **左→右、上→下**。要选第 2 张（index 1）就填 `1.01`（因为值是浮点，稍微给高一点避免落到边界）。
- **SubUV Movie** 模块：按固定帧率循环翻帧（真正的 flipbook）：`Frame Rate`（分布）、`Starting Frame`（**0 = 每粒子随机起始帧**）、`Use Emitter Time`（用发射器时间而非粒子相对时间）、`Use Real Time`。
- **SubUV Images Horizontal / Vertical**：贴图网格列数/行数（如 8×8、4×4）。
- **Particle Cutouts**（可选优化）：给每帧一个不透明度阈值，把透明区域的顶点剔除 → 减少 overdraw。（加法材质用「强度」而非 alpha；社区有 workaround。）

**为什么重要**：一张 8×8 的火 flipbook = 64 帧的火形状演化，粒子随机取相位 → 看不出是同一张图在重复。

### 2.4 Sprite Renderer（广告牌渲染器）

| 属性 | 取值 | 说明 |
|---|---|---|
| **Alignment** | `Unaligned` / `Velocity Aligned` / `Custom Alignment` / `Locked Axis` | 卡片如何对齐 |
| **Facing Mode** | `Face Camera` / `Face Camera Plane` / `Face Camera Position` / `Custom Facing Vector` / `Face Camera Distance Blend` | 朝向哪个方向（`Face Camera Plane` = 面向相机平面，火常用） |
| **Custom Facing Vector Mask** | 1,1,1 等 | 自定义朝向下屏蔽轴 |
| **Sub Image Size** | vec2 | 子图 UV 尺寸（配 flipbook） |
| **SubUV Blending** | on/off | 是否帧间混合 |
| **Sort Mode** | `None / View Depth / View Distance / Custom Sorting` | 半透明排序 |
| **Pivot Offset / UV 空间位置** | — | 旋转/缩放围绕的轴心 |

### 2.5 材质（Material）

- 采样 **Particle SubUV** 节点（flipbook）→ 得到该帧的颜色/alpha。
- 光照模型：火/火花**几乎都是 Unlit（自发光）**（Cascade 文档原文：*Unlit particles (most flames, sparks, etc.)*）。
- **Blend Mode**：`Additive`（加法，发光焰/火花）或 `Translucent`（半透明，烟）。
- **Emissive Color** = 帧贴图 × 颜色（× 温度/黑体色）。
- **UE5 内置黑体（Blackbody）**：给定温度(K)→颜色，用于「温度驱动色相」（比手调 gradient 更物理）。
- **Soft particles**：用场景深度做软边淡出（避免卡片与几何相交的硬边；后续 §5.3 可选）。

---

## 3. Unity 篇（Shuriken + VFX Graph）

### 3.1 Shuriken 模块全景

一个 **Particle System** 组件 = 一组模块：

`Main` / `Emission` / `Shape` / `Velocity over Lifetime` / `Limit Velocity over Lifetime` / `Force over Lifetime` / `Color over Lifetime` / `Color by Speed` / `Size over Lifetime` / `Size by Speed` / `Rotation over Lifetime` / `Rotation by Speed` / `External Forces` / `Noise` / `Collision` / `Triggers` / `Sub Emitters` / `Texture Sheet Animation` / `Trails` / `Custom Data` / `Renderer` / `Lights`。

### 3.2 Main（全局 + 出生状态）

`Duration`（系统时长）、`Looping`、`Prewarm`、`Start Delay`、**`Start Lifetime`**、**`Start Speed`**、`3D Start Size`/**`Start Size`**、`3D Start Rotation`/**`Start Rotation`**、`Flip Rotation`（部分反向旋）、**`Start Color`**、`Gravity Modifier`、`Simulation Space`（Local/World/Custom）、`Simulation Speed`、`Scaling Mode`、`Play on Awake`、**`Max Particles`**、`Auto Random Seed`/**`Random Seed`**（关掉自动 = 可复现）、`Ring Buffer Mode`。

> 每个属性都能用不同的「设置模式」：Constant / Random Between Two Constants / Curve / Random Between Two Curves。**这就是「曲线 / 区间随机」的表达方式。**

### 3.3 Emission

`Rate over Time`（每秒数量）、`Rate over Distance`（每米数量）、**`Bursts`**（在指定 `Time` 喷 `Count` 个，`Cycles` 循环次数、`Interval` 间隔、`Probability` 触发概率）。

### 3.4 Shape（发射形状）

形状：`Sphere / Hemisphere / Cone / Donut / Box / Mesh / Mesh Renderer / Skinned Mesh Renderer / Sprite / Sprite Renderer / Circle / Edge / Rectangle`。

**Cone**（火柱最常用）关键参数：`Angle`（锥角；0=圆柱，90=圆盘）、`Radius`、`Radius Thickness`（0=只表面，1=整个体积）、`Arc`、`Emit from: Base | Volume`、`Length`、`Randomize Direction`（0~1，向随机方向混合）、`Spherize Direction`、`Randomize Position`、`Align to Direction`、以及 `Position/Rotation/Scale` 偏移。

### 3.5 Texture Sheet Animation（**flipbook**）

| 属性 | 说明 |
|---|---|
| `Tiles` | X/Y 方向把贴图切成几块（网格） |
| `Animation` | `Whole Sheet`（整片顺序播）或 `Single Row`（每行一条动画） |
| `Time Mode` | `Lifetime`（按寿命用**曲线**采样）/ `Speed`（按粒子速度）/ `FPS`（按固定帧率） |
| `Row Mode`（Single Row 时） | `Custom` / `Random` / `Mesh Index` |
| `Frame over Time` | **曲线**：帧号随时间推进（可做变速/停顿） |
| `Start Frame` | 起始帧（**用于给每个粒子随机相位**） |
| `Cycles` | 寿命内循环几遍 |

### 3.6 Renderer（**广告牌渲染的核心**）

| 属性 | 取值 / 说明 |
|---|---|
| **Render Mode** | `Billboard`（面相机，球状体积用）/ `Stretched Billboard`（**面相机 + 沿速度拉伸**，加速感）/ `Horizontal Billboard`（平行 XZ 地面）/ `Vertical Billboard`（绕 Y 轴转、竖直，正交相机友好）/ `Mesh` / `None` |
| **Render Alignment** | `View`（面向相机平面）/ `World` / `Local` / `Facing`（朝相机位置）/ `Velocity`（朝速度方向） |
| **Sort Mode** | `None` / `By Distance`（离相机近的在上）/ `Oldest in Front` / `Youngest in Front` / `By Depth` |
| `Sorting Fudge` | 系统间排序偏置 |
| `Min/Max Particle Size` | 屏幕占比上下限（防止过大/过小粒子） |
| `Normal Direction` | 广告牌光照法线（0=当球，1=当平面） |
| `Flip` | 镜像一部分粒子（增加变化） |
| Stretched 附加 | `Camera Scale`（相机运动拉伸）/ `Speed Scale`（速度拉伸）/ `Length Scale`（长度缩放） |

> **Stretched Billboard 的关键**：粒子始终面对相机**并**对齐速度 —— 即使 `Velocity Scale = 0` 也仍对齐速度。

### 3.7 混合（Particle rendering & shading）

- 广告牌可加 **Normal Direction** 做球状着色（配合受光材质，让平面卡片看起来像 3D 粒子）。
- 透明度/发光由 **材质** 决定：加法（发光焰/火花）vs Alpha（烟）。
- **Soft Particles**：粒子与不透明几何相交处按场景深度淡化（Unity 的 Soft Particles 复选框）。

### 3.8 VFX Graph（GPU，现代）

属性化（attributes: position/velocity/color/size/age…）、GPU 模拟、flipbook、subpixel。概念与 Niagara 类似，只是节点图 + 属性数据流。**MVP 不做**，仅作为「未来 GPU 化」的参照。

---

## 4. 引擎无关的「火特效配方」（真正偷到的东西）

### 4.1 分层清单

| 层 | 作用 | 典型参数量级 | 朝向 | 混合 |
|---|---|---|---|---|
| **核心焰 Core flame** | 底部最亮、最热的白/黄 | 少、小、快、寿命短、起始尺寸小 | Billboard / Velocity | **Additive** |
| **主火舌 Main flame** | 主体火舌形状（flipbook 主力） | 中量（~30~80/s）、中大尺寸、向上 | Billboard（Face Camera Plane） | Additive |
| **烟 Smoke ×1~3** | 顶部黑/灰烟，提供体积/深度 | 少量、巨大、寿命长（3~6s）、慢速 | Billboard / World | Alpha（半透明） |
| **火星/余烬 Embers/Sparks** | 小亮点，增强动感 | 少量、极小、快、**受重力回落**、寿命短 | Stretched / Billboard | Additive |
| **热扰动 Heat distortion** | 折射噪波（后期） | — | — | 屏幕空间 |
| **点光 Light** | 照亮周围几何 | 1 个闪烁点光 | — | — |

### 4.2 通用规则

- **颜色带 = 温度梯度**：白热 → 黄 → 橙 → 红 → 暗/透明（气焰根部可带蓝）。**单色橙会被读成「会动的贴花」**。
- **尺寸 over life**：`小 → 大 →（末段）收缩/淡出`。
- **速度**：向上浮力（负重力）+ 锥形扩散 + **噪声湍流** +（火星可选）速度拉伸。
- **相位随机**：flipbook 的 `Start Frame` 随机 + 寿命/速度区间随机 → 消除「节拍感」。
- **分层 > 单体**：与其死磕单个 emitter 的细节，不如叠对层。

### 4.3 具体配方（可直接作 MVP 起点）

来自 Defold 官方 fire+smoke 示例（CC0，**真实数值**，量纲为 Defold 单位，作相对参考）：

**Fire emitter**：Max 128；Circle，Emitter Size X = 100±20（半径）；Spawn Rate = 35±10/s；Initial Speed = 30±20；Initial Size = 80±10；Initial Color = (1.0, 0.5±0.1, 0.1)、Alpha 0.8±0.2；Initial Rotation = 180°（火苗倒放）；外加：Acceleration +70±10、Radial −30±40@y150、Vortex 0±50（火苗抖动）。

**Smoke emitter**：Max 32；位置 y+60；Start Delay 0.5±0.3；Circle Size X=30±10；Spawn Rate = 8±2/s；Lifetime 5；Speed 10±10；Size 40±20；Alpha 0.5±0.3；Initial Rotation 0±90；外加 Acceleration +20±10、Radial −40±20@y150。

**层叠顺序**：火在前（Z=0.1）、烟在后（Z=0）。

---

## 5. 落到 JPOV 的设计

### 5.1 数据模型（命令层）

沿用 JPOV「命令描述 what、渲染器描述 how」的分离（见 SOUL「Separate What from How」）。

```cpp
// 一条命令 = 一个粒子系统（可含多个发射器 = 分层）。
// 与 InstancedObjectCommand / SkinnedMeshCommand 平级，进 RenderCommandList::particles。
struct ParticleSystemCommand {
    std::vector<ParticleEmitter> emitters;
    // 系统级：模拟空间（World/Local）、随机种子（确定性）、时间缩放…
    bool world_space = true;
    uint32_t seed = 0;          // 可复现
};

struct ParticleEmitter {
    // ── 发射 ──
    SpawnSpec   spawn;          // rate / burst / 形状
    ShapeSpec   shape;          // kCone / kCircle / kBox / kPoint；angle/radius/thickness/arc…
    // ── 出生状态（Main）──
    LifeSpec    life;           // lifetime(min,max)
    Vec3f       start_speed;    // 或标量 + 方向散布
    RangeF      start_size;     // (min,max)
    RangeF      start_rotation; // (min,max)，度
    Gradient    start_color;    // 出生颜色（可单色）
    Vec3f       gravity;        // 火：常给 +Y 浮力（负重力）
    // ── 生命周期演化 ──
    Vec3f       velocity_over_life;   // 线性漂移
    RangeF      radial;               // 径向速度
    NoiseSpec   noise;                // 湍流（频率/强度）
    Gradient    color_over_life;      // 颜色带（温度梯度）
    Curve       size_over_life;       // 尺寸曲线
    Curve       alpha_over_life;      // 不透明度曲线（可选，合并进 color）
    // ── flipbook ──
    TextureSheet sheet;         // tiles_x, tiles_y, time_mode(Lifetime/FPS/Speed),
                                // frame_curve, start_frame(0=random), cycles, blend
    // ── 渲染 ──
    BillboardMode align;        // kView / kWorld / kFacing / kVelocity
    bool stretched = false;     // 沿速度拉伸
    BlendMode blend;            // kAdditive / kAlpha
    uint32_t texture_id;        // flipbook 纹理（共享，TextureManager 注册）
    SortMode sort;              // kNone(加法) / kByDepth(半透明)
    bool soft_particle = false; // 场景深度软边（可选）
};
```

**曲线/渐变**：`Curve`（`vector<pair<float,float>>` key: t∈[0,1]→value）与 `Gradient`（key: t→Color）；区间随机用 `RangeF{min,max}`。语义对齐 UE Distributions / Unity MinMaxCurve。

### 5.2 运行时（粒子状态 + 模拟）

- 每 `Particle`：`pos, vel, age, lifetime, size0, rot0, rot_vel, color0, seed, frame_phase`。
- **模拟在 CPU**（MVP）：粒子量小（几百~几千），确定性好，llvmpipe 友好；seed 驱动随机、无浮点竞态 → 可复现截图。
- 每帧 `age += dt`；`vel += (gravity + noise) * dt`；`pos += vel * dt`；超寿命回收。**生成端用带种子的确定性 RNG**。
- 阶段 2：GPU 模拟（transform feedback / compute），命令层接口不变，只换后端。

### 5.3 渲染

- **一个发射器 = 一次 instanced draw**：共享一个**单位四边形** mesh（2 tri，VAO），per-instance attribute 传：
  `center(世界) / size(vec2) / rotation(float) / color(rgba) / subuv(frame, tiles) / flip`。
  复用现有 `InstanceBuffer`（per-instance 布局 spec + Attach/Detach 配对语义，见 `instance_buffer.h`）。
- **Billboard 朝向**：VS 里按 `align` 构造面片基向量（`kView`=面向相机平面；`kFacing`=朝相机点；`kVelocity`/`stretched`=沿速度）。
- **Flipbook**：UV 由 `(tiles_x, tiles_y, frame_index)` 裁出子图；可选**相邻帧混合**（UE `PSUVIM_Linear_Blend`）。
- **软粒子**：采样 **MRT#1 场景深度**（float 颜色纹理，llvmpipe 不能采 depth texture —— 既有约定）→ 与当前片元深度比较淡化（MVP 可略）。
- **混合**：`kAdditive` = `(GL_SRC_ALPHA, GL_ONE)`（或预乘 `GL_ONE, GL_ONE`）；`kAlpha` = `(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`。
- **排序**：`kAlpha` 层按视深**背→前**逐实例排序；`kAdditive` 层**不排**（无顺序依赖，省排序）。层间顺序 = 命令内 emitter 顺序（先烟后焰？错 —— **先画烟、后画焰**让焰压在烟上；加法层通常在 alpha 层之后画）。
- **HDR**：输出到 3D HDR FBO，可被现有 **bloom** 消费 → 火自动发光。
- **GL 状态机**：自行保存/复原 FBO / viewport / 活动纹理单元 / blend func（同 fog 约定；调用方 `glPushAttrib` 兜底）。
- **集成点**：新 `src/particles/particle_renderer.{h,cc}`（与 object3d / skeleton / fog 平级、单一自包含）；`Renderer` 调它；拾取/高亮 **MVP 不接**（后续再加）。

### 5.4 资源（flipbook 火序列图）

- **首选：自产**。我们**已有 Mantaflow 烘的 VDB 火**（`/james_pm/fire_hero/assets/`）。写一个离屏渲染器把序列体**渲成序列帧 → 拼成 sheet（如 8×8 / 4×4）**，做无缝循环。优点：可控、免授权、与体积火资产同源。
- **备选：CC0 包**（Unity《Flame Pack》等 free flipbook；freecutout / ambientCG 的粒子图）。
- **纹理格式**：建议 `RGBA8`，`RGB = 预乘温度色 / 强度`、`A = 不透明度`（加法用 RGB 强度，alpha 用 A）。约定写进材质/命令注释（单一来源）。

### 5.5 viewer 面板（调参）

系统级：播放/暂停/时间/fps/seed。每 emitter：spawn rate、形状、寿命、速度、尺寸曲线、颜色带、噪声强度、flipbook tiles/帧率/起始帧、朝向、混合、拉伸开关。

---

## 6. 成本模型

- 成本 ∝ **Σ(每 emitter 粒子数) × 平均屏幕覆盖（overdraw）**。加法混合的 overdraw 是主要开销（全屏加法会爆，但火的覆盖区域不大）。
- llvmpipe：几百粒子/层可交互预览；真 GPU：轻松上万。
- 降本手段：`Max Particles` 上限、Cutout 剔除透明顶点（§2.3）、LOD、屏幕尺寸上下限（§3.6）。

---

## 7. 里程碑（下期 MVP = M1）

- **M1（下期）**：单 emitter，`Cone` 形状，**加法混合**，flipbook（tiles 切帧 + 帧率 + 起始帧随机），`size_over_life` / `color_over_life` 曲线，`kView` 朝向 → 在新 viewer 里渲出一根**像样的火柱**。目标：验证「广告牌 + flipbook + 曲线」这条链在 JPOV 跑通且好看。
- **M2**：**分层**（核心焰 + 主焰 + 烟 + 火星），混合模式 + 排序，颜色带细化。
- **M3**：软粒子、速度拉伸（Stretched Billboard）、噪声湍流、可选点光。
- **M4**：GPU 模拟、GPU 粒子、LOD、拾取/高亮集成。

---

## 8. 复用现状 & 与既有管线的关系

**复用（别重造）**：
- `InstanceBuffer`（per-instance 布局 spec + Attach/Detach 配对）与既有 instancing 链路。
- HDR / bloom / MRT 场景深度（软粒子用）/ 混合状态机约定（fog 同款）。
- viewer 框架（`demo/*_viewer_app.h`、`jpov_*_viewer.cc`、build 脚本模式）。
- **Mantaflow 烘焙管线**（`tools/jpov/genVolume/` + `env/`）→ 出 flipbook 序列帧。

**分工**：
- **环境雾 / 烟 / 远景火** → `fog`（froxel，已实现）。
- **主角「燃料火」** → **本方案（广告牌粒子）**。
- **逐像素体渲染**（`fire_hero/DESIGN.md`）→ 可选增强，暂缓。

---

## 9. 参考来源

- UE Cascade Particle Systems / Key Particle Concepts / SubUV Modules / Particle System Reference（Epic 官方文档）。
- UE Niagara Overview / Particle Spawn Group Reference / System and Emitter Module Reference（Epic 官方文档）。
- Unity Manual：Main / Emission / Shape / Velocity over Lifetime / Color over Lifetime / Texture Sheet Animation / Renderer / Particle rendering and shading 模块参考。
- Defold「fire and smoke」官方示例（CC0，含具体数值）。
- Godot 4 GPUParticles flipbook（交叉验证 flipbook 参数：H/V Frames + Speed）。
- 观察：AAA 主角火普遍为「分层广告牌粒子 + flipbook」（例：Disney「From Spark to Inferno: Scalable Stylized Fire」的 layered elements：hero fire / mid-ground / dressing fires / embers / smoke；Daniel Swahn 的 UE4 Niagara billboard + flipbook 火，色相由色温(K)推）。

---

## 10. 附录：UE / Unity 参数速查表

**UE Cascade 模块类别**：Acceleration, Attraction, Collision, Color, Event, Kill, Location, Material, Orbit, Rotation, Size, Spawn, SubUV, Velocity。
**UE SubUV**：InterpolationMethod（None/Linear/Linear_Blend/Random/Random_Blend）、SubImage Index（float，RelativeTime）、SubUV Movie（Frame Rate / Starting Frame 0=random / Use Emitter Time）、Images Horizontal/Vertical。
**UE Niagara 组**：Emitter Spawn / Emitter Update / Particle Spawn / Particle Update / Render。Sprite Renderer：Alignment（Unaligned/Velocity Aligned/Custom/Locked Axis）、Facing Mode（Face Camera/Face Camera Plane/Face Camera Position/Custom Facing Vector/Distance Blend）、Sub Image Size、Sort Mode。

**Unity Shuriken 模块**：Main, Emission, Shape, Velocity over Lifetime, Limit Velocity over Lifetime, Force over Lifetime, Color over Lifetime, Color by Speed, Size over Lifetime, Size by Speed, Rotation over Lifetime, Rotation by Speed, External Forces, Noise, Collision, Triggers, Sub Emitters, Texture Sheet Animation, Trails, Custom Data, Renderer, Lights。
**Unity Shape**：Sphere/Hemisphere/Cone/Donut/Box/Mesh/Mesh Renderer/Skinned Mesh Renderer/Sprite/Sprite Renderer/Circle/Edge/Rectangle。
**Unity Texture Sheet Animation**：Tiles(X,Y)、Animation(Whole Sheet/Single Row)、Time Mode(Lifetime/Speed/FPS)、Frame over Time(curve)、Start Frame、Cycles。
**Unity Renderer**：Render Mode(Billboard/Stretched Billboard/Horizontal Billboard/Vertical Billboard/Mesh/None)、Render Alignment(View/World/Local/Facing/Velocity)、Sort Mode(None/By Distance/Oldest in Front/Youngest in Front/By Depth)、Min/Max Particle Size、Normal Direction、Flip。
**属性设置模式（Unity）**：Constant / Random Between Two Constants / Curve / Random Between Two Curves。

---

## 11. 专项：软粒子（软深度检测）与广告牌朝向调制

> 本节按**查明状态**分级标注：`✅已查明` = 有标准做法/公式且 JPOV 落点清楚；`⚠️待验证` = 业界有成熟解法但 JPOV 未验证，或需设计决策。

### 11.1 软粒子 / 软深度检测（Soft Particles / Depth Fade）—— `✅已查明`

**问题**：广告牌是平面卡片，与不透明几何相交时会露出**硬切边**（火插进地面/物体时最明显）。

**标准做法（已查明，公式固定）**：把粒子片元深度与**场景不透明深度**比较，靠近时不透明化：
```
softZ(screen uv) = 场景不透明线性深度  // 在粒子之前渲染，故不含粒子
particleZ       = 当前片元线性深度    // gl_FragCoord.z → 线性化
fade            = saturate((softZ - particleZ) / softness)   // softness = 淡出距离(世界单位)
src_a *= fade            // 加法混合：乘 RGB；alpha 混合：乘 A
```
- Unity：Renderer 的 **Soft Particles** 勾选（需相机 depth texture）；UE：材质 **Depth Fade**（或 Pixel Depth Offset 做写深度）。
- **JPOV 落点（已明确）**：
  1. **深度来源 = MRT#1 的场景深度颜色纹理（float）**，不是 depth texture —— llvmpipe 不能采 depth texture，这是既有约定（fog 同款）。绑定 resolve 后的场景深度纹理给粒子 shader 采样。
  2. **绘制顺序**：粒子**必须在所有不透明几何之后**画，此时 MRT#1 深度 = 纯不透明深度（透明不写深度）。
  3. **只对不透明几何淡出**（天然满足：透明层不写深度）。
  4. **面板参数**：每 emitter `soft_distance`（世界单位，0 = 关，等价 Unity 的软边因子）。
- **成本**：每次片元多一次纹理采样，可忽略。
- **状态**：**无需再调研，属实现细节**。MVP 可先不做（M1），M3 接。

### 11.2 广告牌朝向调制（Billboard Orientation Modulation）

**A. 基础模式 / 公式 —— `✅已查明`**（图形学标准做法）

记相机位置 `C`、粒子位置 `P`、`d = normalize(P − C)`：

| 调制 | 基向量构造 | 语义 / 对应 | 火里用途 |
|---|---|---|---|
| **屏幕/view-plane 对齐** | `right = cross(worldUp, d)`；`up = cross(d, right)` | 卡片平行于相机平面 | 通用（但会随相机 roll 扭） |
| **球面 facing（朝相机点）** | 卡片法线指向 `C`（逐粒子朝向略不同） | UE `Face Camera Position` | 圆团状焰/烟 |
| **竖直/圆柱对齐（roll 锁定）★** | `up = worldUp` 固定；`right = cross(worldUp, d)`；**只绕 Y 轴 yaw** | Unity `Vertical Billboard` / UE `Face Camera Plane`+轴锁 | **火的关键**：卡片保持竖直，不随相机翻滚/扭转 |
| **world / local 固定** | 用世界/局部轴 | UE `Unaligned` / 对齐世界轴 | 平铺地面烟/贴花 |
| **velocity 对齐** | `up = normalize(vel)`；`right = cross(up, d)`（再正交化） | Unity `Render Alignment = Velocity` | 火花/余烬 |
| **stretched（沿速度拉伸）** | 在 velocity 对齐基础上，长度 × `speed·speedScale`、宽 × `cameraScale` | Unity `Stretched Billboard` | 快速火星拉成条 |
| **per-particle roll** | 卡片平面内再转 `θ` 角（`start_rotation` + `rotation_over_life`） | 通用 | 增加变化 |
| **custom facing mask** | UE `Custom Facing Vector Mask` 屏蔽轴 | UE `Custom Facing Vector` | 特例 |

**B. 组合 + 边界 —— `⚠️待验证`（业界有解法，JPOV 未验证；需设计决策）**

1. **退化保护**：`d ∥ worldUp`（俯视火焰）或 `vel→0`（速度拉伸）时会 `cross` 出零向量 → 需稳定 fallback（如退化时回退 view-plane / 关闭拉伸），否则 NaN / 闪烁。
2. **roll-lock 在正上/正下方的稳定性**：`d` 接近 `worldUp` 时 `right` 方向不稳 → 需限位或平滑。
3. **velocity 拉伸 + roll 锁定叠加**：两者对 `up` 的需求冲突，需定义优先级/混合权重。
4. **轴穿越 popping**：朝向跨过某个轴时可能突跳，需连续化（或接受、用 roll 抖动掩盖）。
5. **光照法线**：广告牌平面的法线 vs 球状法线（Unity `Normal Direction` 0=球/1=平面）—— 火多为 unlit 可忽略；受光粒子需选择。

**结论**：调制**模式**是查明的，可照抄；**「火该用哪几种 + 怎么组合 + 退化/popping 处理」待 M2/M3 实现时验证并回填本文档**。MVP(M1) 只需 **roll-locked 竖直广告牌**（避免翻滚）+ 可选 per-particle roll 即可。

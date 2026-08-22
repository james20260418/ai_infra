# JPOV 光源强度（intensity）物理锚点约定

> 目标：让每一种光源的 `intensity = 1.0` 都有一个**明确的物理含义**，使各光源
> 的数值得以相互对标、彼此理解，而不是"谁亮谁暗靠手感和拍脑袋"。
>
> 现在 JPOV 是 HDR 渲染（RGBA16F FBO + 后处理 tone map），颜色的 RGB 分量本身
> 可以承载 >1 的亮度，因此**亮度量级与色调（色温）应当分离**：
>   - **RGB / color**：只表达**色调/色温**，约定范围 [0,1]，不承载绝对亮度。
>   - **intensity（float 标量）**：承载**绝对亮度量级**，`intensity = 1.0` 对应
>     下面表格里每种光源各自的物理锚点。

---

## 一、统一锚点场景（Anchor Scene）

> 一个可复用、可计算的基准环境，用来定义"intensity = 1.0"到底有多亮：

**正午、晴朗蓝天**，一座亭子的**阴影区**里点亮一只 **100 W 白炽灯泡**。

在这个场景里，四类亮度来源同时存在，且各自独立锚定：

| 亮度来源 | intensity = 1.0 的物理含义 | 依据 |
|---|---|---|
| **sun**（太阳方向光） | 正午晴空**直射太阳** ≈ 100,000 lux | Wikipedia / Trilux |
| **sky**（蓝天散射） | 正午晴空**天顶天空**的散射亮度 | Preetham Y_z（见下） |
| **ambient**（环境光） | 正午晴空**亭子阴影**里的环境光 ≈ 20,000 lux | Wikipedia "shade illuminated by clear blue sky, midday = 20,000 lux" |
| **point light**（点光源） | 一只 **100 W 白炽灯泡**（≈ 1600 流明） | energy.gov：100W 白炽灯 ≈ 1600 lm |

> 注意：同一时刻、同一场景里，`sun.intensity=1`（直射 100k lux）和
> `ambient.intensity=1`（阴影 20k lux）**本来就差 5 倍**——这是物理事实，不是
> 矛盾。两者是不同物理量（方向直射 vs 半球散射），各自独立锚定，用户按需取值。
> "正午阴影点灯泡"是给这四者**各自**一个可想象的物理参照，不是要求它们数值相等。

---

## 二、各光源 intensity 锚点细则

### 1. 太阳方向光 `DirectionalLight`
- `direction`：光传播方向（单位向量，y-up 世界）。
- `color`：**色温语义**，只表达色调。
  - 正午 ≈ 中性白（约 5600K）；
  - 日出/日落 ≈ 暖橙红（约 2000~3000K）。
- `intensity = 1.0` ≈ **正午晴空直射太阳 ≈ 100,000 lux**。
- 低角度太阳的强度衰减由**经验照度表**决定（不再用 Beer-Lambert 解析拟合，见下）。
- **自动推导 `DaySkyCommand::DirectionalIntensity()`（2026-08-22 改）**：从 `sun_dir` 仰角
  查经验照度表，用 `geom::math::PiecewiseLinearFunction` 分段线性插值：
  `intensity = midday_intensity × 衰减系数(仰角)`，`midday_intensity` 默认 3.0。
  经验锤点（晴天直射 + 天光，单位：太阳仰角° → 相对正午照度系数）：
    `0°→0.01  5°→0.04  10°→0.10  20°→0.25  30°→0.40  60°→0.80  90°→1.00`
  仰角 <0° / >90° 时 PWL 夹断到端点系数（0.01 / 1.00），不随越界外推。
  turbidity **暂时忽略**（不影响方向光强度，只影响天色/太阳盘）。
  - 背景：早期用 Beer-Lambert `exp(−τ·AM)`（`AM=1/sin(elev)`）解析拟合，但该式在低仰角
    会因 AM 发散（大气球壳 d2 有上界，并不无限增长），散射主导时纯吸收模型失真，
    衰减趋势也偏离实际照度，故改为直接查经验照度锚点插值。
  - 用法：`light.intensity = sky.DirectionalIntensity()`（颜色仍用 `DirectionalColor()`，
    两者配套，见 `DaySkyCommand` 注释）。
- 月光方向光：物理上月光 ≈ 阳光的 1/400,000，即 `intensity ≈ 2.5e-6`
  （满月地面照度 ≈ 0.3 lux / 100,000 lux）。配合冷色（约 4100K）。

### 2. 天光背景 `DaySkyCommand`
- `sun_dir / turbidity / season / ground_color`：见 DaySkyCommand 注释。
- `season`：**只调天空的色温气氛**（多分量乘子），不染太阳盘（太阳盘是自发光天体，
  色调由仰角散射决定，不受季节色温染色）。
- `intensity = 1.0` ≈ **正午晴天的蓝天背景**（已定标，2026-08-19 Danis 确认）。
  - 归一化系数 `SKY_LUMINANCE_SCALE = 0.04`（`sky_renderer.h`）已定：把 Preetham
    的物理天顶亮度（~几千 cd/m²）压到 JPOV HDR 标尺，使 intensity=1.0 时天空为
    正常蓝天渐变（顶部深蓝 → 地平线浅蓝发白）。
  - 配合 `sun=3` / `ambient=0.3`（见三·五 晴天基准值）+ ACES tone mapping，同一
    HDR 标尺、协调一致。

### 3. 全局环境光 `AmbientLight`
- `color`：环境光**色调**（天空日间可偏蓝，洞穴岩浆可偏橙，夜空偏深蓝）。
- `intensity = 1.0` ≈ **正午晴空亭子阴影里的环境光 ≈ 20,000 lux**。
- 环境光无方向、无影子，是 PBR 的 `ambient × base_color × AO` 项。
- 参考：Godot 官方 `Ambient Energy 0.3~0.8`（"lower = harder shadows"）；
  LearnOpenGL/PBR 常数环境项 ≈ 0.03（相对光源 1.0）。JPOV 以 20k lux 阴影为
  1.0 锚点，其他时刻/天气按比例缩放（夜晚远小于日间）。

### 4. 点光源 `PointLight`
- `color`：**色温语义**（灯泡暖白 ~2700K，火光橙红，霓虹彩光等）。
- `intensity = 1.0` ≈ **一只 100 W 白炽灯泡（≈ 1600 流明）**。
- 点光源的衰减（inverse-square 或线性）由 `linear_radius` / 后续衰减函数处理，
  `intensity` 之外的几何/距离关系已经独立，`intensity` 只锚定"这个光源发光总量"。
- 物理半径 `physical_radius`（米）：Representative Point 球面光，默认 0=纯点光。
  - 典型值：灯泡 ~0.02–0.05 m，方灯 ~0.5 m。
- 参考换算：100W ≈ 1600 lm；1 lux = 1 lm/m²；距离 1m 处（整球 4π sr）照度
  ≈ 1600/(4π) ≈ 127 lux（未计反射/光学损失）。

---

## 三、归一化系数（理论值，供 step 2 调参对标）

> 这是“intensity=1.0 物理锚点”换算出的**相对亮度系数**，是重构后统一
> 亮度标尺的基准。所有测试 initial 都给 intensity=1.0，下面是它们理论应取
> 的相对关系。

### 相对系数（以正午直射太阳 = 1.0 为基准）

| 光源 | intensity=1.0 物理锚定 | 相对 sun.intensity=1.0 的系数 |
|---|---|---|
| **sun**（直射太阳） | 100,000 lux | **1.0**（基准） |
| **ambient**（蓝天阴影） | 20,000 lux | **0.2**（= 1/5） |
| **point light**（100W 灯泡 @1m） | ≈ 127 lux | **≈ 0.00127**（= 1/787，局部光） |

> 关键结论：**sun : ambient = 5 : 1**。即同一锚点场景里，`sun.intensity=1`
> 照亮的表面应比 `ambient.intensity=1` 仅靠环境光照亮的表面亮 5 倍。
> 这是 step 2 调参时验证“ambient 相对系数是否正常”的核心判据。

### 基准换算依据

- 100W 白炽灯 = 1600 lm；均匀球面（4π sr）1m 处照度：
  `1600 / (4π) ≈ 127 lux`。
- 正午直射太阳 ≈ 100,000 lux（Wikipedia/Trilux）。
- 正午蓝天阴影 ≈ 20,000 lux（Wikipedia：clear blue sky shade at midday）。

### 后续待办（非本次 PR scope）

- **点光源衰减**：当前 linear 衰减（`1−dist/radius`）非物理 inverse-square，数值与
  sun/ambient 不可直接对标，留待后续。
- **primitives3d LDR 通道**：深度共享语义已定，后续按需拆分。

---

## 三·五、晴天正午基准值（已定，供直接使用）

> 前提：**必须开启 ACES tone mapping**（`RenderCommandList::tone_mapping = true`）。
> 因为 ACES（luminance-only）是非线性的，只有在**线性区**（表面 radiance ≈ 0~0.6）
> 内，5:1 的物理对比度才能被保真呈现；基准值取得太大，5:1 会被曲线压缩到 ~1:1
> 导致影子消失。

### 结论：`sun : ambient = 5 : 1`，且绝对取值要落在 ACES 线性区

| 光源 | intensity 取值 | 相对关系 | 物理锚定 |
|---|---|---|---|
| **DirectionalLight（太阳）** | **3.0** | 基准 | 正午直射太阳（约 120,000 lux）|
| **AmbientLight（环境光）** | **0.3** | 1/5（≈ 0.26~0.3）| 正午晴天阴影（约 20,000 lux）|

> - 这是 2026-08-19 在 `scene_in_sun` 场景实测 + 理论反推共同确定的晴天正午基准。
> - 兑换依据：对水平白地面（base_color=1，NdotL≈0.577），surface radiance
>   = `intensity × (kD/π) × NdotL ≈ intensity × 0.176`。要 high-light≈0.5、
>   shadow≈0.1（都在 ACES 线性区）→ 反解 `sun≈2.8`、`ambient≈0.3`。
> - **业界标准确认（Unity HDRP / Unreal 官方）**：directional light = 120,000~130,000
>   lux，skylight ≈ 20,000~25,000 lux（≈ sun 的 20%），即 5:1~6:1。JPOV 因无
>   exposure 系统，用相对单位（sun=3, ambient=0.3）替代绝对 lux，比例保持一致。

### 为什么不能直接填绝对 lux

- ACES 曲线对 x>~10 就饱和到 1.0（`aces(10)=1.0, aces(120000)=1.0`）。
- 若直接填 `sun=120000, ambient=20000`，两者都被曲线压平到 1.0，5:1 完全消失。
- 所以必须用“相对单位”，且把基准落在 ACES **线性区**（surface radiance 峰值 ≈ 0.5）。

### ACES 线性区参考（决定取值上限）

| surface radiance（tone map 前）| ACES 输出 | 备注 |
|---|---|---|
| 0.1 | 0.13 | 阴影基准 |
| 0.5 | 0.62 | 高光基准（线性区上限附近）|
| 1.0 | 0.80 | 开始明显压缩 |
| 5.0 | 0.99 | 已接近饱和 |
| ≥10 | 1.00 | 完全饱和 |

> ⚠️ **点光源暂不按此表定值**：当前点光源用线性衰减（`1 - dist/radius`），
> 非物理的 inverse-square，数值与 sun/ambient 不可直接对标，留待后续改衰减函数
> 时一并处理。

---

## 四、量级速查表（用于快速对标）

| 场景 | 照度（lux） | 相对 sun.intensity=1 的倍数 |
|---|---|---|
| 正午直射太阳 | 100,000 | 1.0（锚点） |
| 正午蓝天阴影 / 环境光 | 20,000 | 0.2 |
| 阴天正午 | 1,000–2,000 | 0.01–0.02 |
| 日出/日落 | ~400 | ~4e-3 |
| 100W 灯泡 @1m | ~127（简化） | ~1.3e-3（但这是"局部"而非全局） |
| 满月月光 | ~0.3 | ~3e-6 |

> ⚠️ 点光源的"局部照度"与全局光照（sun/ambient/sky）不是同一维度，不能直接比
> 数字大小；点光源的意义是"在它衰减半径内的局部补光"。上表的点光源行仅供
> "灯泡发光总量"的直觉参照，不参与全局强度排序。

---

## 五、设计原则

1. **色调与亮度分离**：`color`（RGB）只表达色温/色调，约定 [0,1]；亮度量级
   全部收敛到 `intensity` 标量。这样 `PointLight.color=(1,0.8,0.6)` 与
   `sun.color=(1,1,1)` 都是"纯色调"，谁亮谁暗看 `intensity`。
2. **intensity=1.0 必有物理锚点**：见上表，不允许"没含义的随手值"。
3. **所有光源都要有 intensity**：现状 PointLight 缺、AmbientLight 用
   `strength`（语义混乱），需统一为 `intensity`。
4. **HDR 同一亮度标尺**：天空、太阳、环境光、点光源的输出最终要在同一条
   HDR 数轴上，交给统一的 tone map pass 压缩；不允许"天空几百、物体几"的
   量级脱节。天空（`SKY_LUMINANCE_SCALE=0.04`）已与 sun/ambient 定标对齐
   （见三·五 + 第二节）。

---

## 六、待办 / 迁移清单

- [x] `PointLight` 增加 `intensity` 字段（color 退化为色温语义）。
- [x] `AmbientLight.strength` → `intensity`（统一命名）。
- [x] `DirectionalLight` 保留 `intensity`，确认锚点 = 100k lux 正午直射。
- [x] 现有带光照 test 全部给成 intensity=1.0（color 改成纯色温 1,1,1）。
- [x] `DaySkyCommand` 厘清 `intensity` 与内部 `SKY_LUMINANCE_SCALE` 关系，
      把天空归一到与物体光照同一标尺（`SKY_LUMINANCE_SCALE=0.04` 已定，
      intensity=1.0 = 正午晴天，见三·五 + 第二节）。
- [x] 太阳盘参数化：`sun_radius`（角半径，物理 2×）+ `sun_brightness`（自发光
      亮度基数，物理 2×10⁵）+ `sun_glow`（光晕强度，独立 HDR），色温/衰减从
      sun_dir + turbidity 推导（见第八节）。

---

## 七、物理数据来源

- 正午直射太阳 ~100,000–120,000 lux：Wikipedia "Daylight"、Trilux。
- 正午蓝天阴影 ~20,000 lux：Wikipedia "Daylight"（"Shade illuminated by entire
  clear blue sky, midday"）。
- 100W 白炽灯泡 ~1600 lumen：energy.gov "Lumens and Lighting Facts Label"。
- 月光 ~0.1–0.3 lux：Trilux（0.1 lux）、Oxford 实测（0.26–0.30 lux 满月）。
- 蓝天色温 ~20,000K、太阳 ~5,000K、月光 ~4,100K：Trilux。

---

## 八、太阳盘（自发光天体）参数设计

太阳盘是自发光天体（5778K 黑体辐射），Preetham 散射模型不含它，故需单独参数。
与“色温/衰减可推导”的其它项分开：

| 属性 | 来源 |
|---|---|
| 颜色（色温 2000K 日出→5600K 正午）| ✅ 由仰角推导（colorTempToLinear）|
| 亮度随仰角衰减 | ✅ Beer-Lambert `exp(−τ·AM)`，AM≈1/sin(仰角)，τ 由 turbidity |
| 光晕宽窄 | ✅ 由 sun_radius 决定（σ≈2×radius）|
| **半径** | ❌ 必须单独给 `sun_radius` |
| **亮度基数** | ❌ 必须单独给 `sun_brightness` |
| **光晕强度** | ❌ 必须单独给 `sun_glow`（艺术参数）|

### 参数与默认值

| 参数 | 默认 | 依据 |
|---|---|---|
| `sun_radius` | 0.0094 rad（0.54°）| 真实太阳角半径 0.27° 的 2 倍，低分辨率下更醒目 |
| `sun_brightness` | 2.0×10⁵ | 太阳盘亮度 1.6×10⁹ / 晴天天空 8000 cd/m²（均 radiance，可直接比）|
| `sun_glow` | 1.0 | 光晕强度，独立 HDR（量级 ~几），非与盘亮度同源 |

> `sun_radius≤0` 或 `sun_brightness≤0` = 不画盘；`sun_glow≤0` = 无光晕。

### 两个实现要点（重要，踩坑结论）

1. **盘 mask 用角度空间 smoothstep，不用 cos 空间**：
   经典 `smoothstep(cosSA, 1.0, cos_theta)`（Godot Freeman Sky 等）在 cosSA 接近 1
   （小太阳盘）时失效——cos 在 θ→0 处斜率→0，cos 空间分辨率极不均匀，过渡被挤成
   1 像素硬边。正确做法：先 `ang=acos(cos_ang)` 再 `smoothstep(0, uSunRadius, ang)`。
2. **光晕独立强度，不与盘亮度同源**：光晕是大气散射，物理强度 ~盘的 1e-4~1e-5。
   若写成 `sun_brightness × glow`（基数 1e3~1e5），盘外几像素仍被顶到 ACES 饱和，
   高斯衰减看不出渐变。正确：光晕用独立的 `uSunGlow`（量级 ~1~几）做绝对 HDR 强度，
   `glow = uSunGlow × exp(−d²/2σ²)`，σ≈2×sun_radius。

### 光晕公式（标准高斯辉光）

```glsl
float d = max(ang - uSunRadius, 0.0);   // 距盘边缘的角距离
float sigma = uSunRadius * 2.0;          // 光晕宽度 ∝ 盘大小
float glow = uSunGlow * exp(-(d*d) / (2.0*sigma*sigma));
```


# JPOV 远景雾（空气透视）—— froxel 里的全局介质

> 状态：**已实现（2026-11-18，PR #263）**。作者：James / Danis。
> 关联：`jpov_froxel_design.md`（场/inject/scatter/合成）、`jpov_water_contract.md`（雾只在水上）、
> `jpov_fog_design.md`（点状雾团）。代码入口：`interface/render_command.h::DistanceFog`。

## 0. TL;DR

远景雾不再是「屏幕空间解析式 + 一次全屏 alpha 混合」，而是**一块真实体积雾**：
由 froxel inject 逐切片累加 `(τ, S)`，与雾团、水体介质、透明层在**同一次合成趟**里
按深度交替积分。它因此天然享有整条链路的既有性质：按真实几何深度裁剪、与层正确排序、
逐通道介质相加、被水体契约约束。

```
Δτ = σ_t · w(P) · Δlen            // P = 该 froxel 切片内的段中点
S  = color · (1 − e^{−Δτ})        // 内散射色 = 用户给定，**不吃光照**
over 合成 ⇒ 整条视线 S = color·(1 − e^{−τ_total})：无限远收敛到 color
```

## 1. 为什么换掉解析式

旧的解析远景雾（`ElevationFogConfig` + `HorizonFogRenderer`）按
`τ = density · band(|d.y|) · smoothstep(start, full, dist)` 逐像素算 α，然后在 3D 之后整屏
alpha 混合。三条硬伤：

1. **看不见「层」**。它只是一次全屏后盖：条带（strip WBO）在它之前合成、水皮在它之后画，
   于是「条带被雾了、水皮没被雾」，而且顺序改一次就换一种错法。
2. **视角依赖**。`band` 是 `|视线仰角|` 的函数 —— 同一世界位置，主视线和折射射线拿到不同
   密度；要接折射/反射就必须先把这个假拆掉。
3. **与其它介质不可交换**。解析 α 只能"后盖"，没法参与 `τ = τ_fog + τ_water` 这种前缀累加，
   也没法按透明层的代表深度切段。

换成体积介质后，这三条同时消失：场就是 (τ, S) 前缀，合成器本来就认这个字母表。

## 2. 形状：为什么还是「圆角盒」

沿用 `PointFog` 的容器（内盒 + 外侧球面圆角壳 + 剖面），因为：

- **软边免费**。`radius` + `attenuation`（`kLinear` / `kExponential` / `kQuadratic`）都是
  连续剖面，盒面上没有硬缝；`kUniform`（w ≡ 1）是硬边，**配了会 CHECK 打回**。
- **零新代码路径**。inject 里把 `DistanceFog` 组装成一个 `FogBodyData`（kind = `kRoundBox`、
  轴 = 世界单位轴），直接调**同一个** `SampleFogSegment` —— 求交、剖面、切片裁剪全部复用。
- **一次配置能表达两种常见形态**：薄板（水平大、竖直薄）= 贴地雾层；
  大立方 = 均匀雾。

## 3. 与点状雾团的两点语义差异

| 维度 | `PointFog`（团） | `DistanceFog`（远景雾） |
|---|---|---|
| 覆盖 | 局部盒，屏幕 tile 剪枝 + 每 tile ≤8 个预算 | **全局**：不剪枝、不占预算，每帧存在于所有 froxel |
| 源项 | `albedo ⊙ L_light + ε`（ambient + 太阳×CSM×相位 + 点光） | **`color · (1−e^{−Δτ})`**：用户给什么色就是什么色，不消费任何光 |
| 关闭 | `point_fogs` 为空 | `enabled=false`（等价于不传） |

「不吃光照」是刻意的：`color` 就是**收敛色**（视线无限远时画面呈现的颜色），语义与
「天空辐射亮度」同量纲。要不要乘方向性天光/太阳相位**不是**这个接口的事 —— 需要时那是
另一个特性（见 §7）。

## 4. 落点（一张图）

```
inject（froxel）:
    ro/rd = tile 中心射线
    acc = 0
    ├─ 远景雾：SampleFogSegment(DistanceFogBody) ⇒ (σ, [t0,t1])
    │          Δτ = σ·len，S = color·(1−e^{−Δτ})        ← 不吃光照
    └─ 团循环（原样）：ambient + 太阳×CSM×相位 + 点光
reduce / scatter：**一行没改**（场里只是多了一项 τ/S）
composite：**一行没改**（FogField + 透明层 + 水体介质照旧）
```

inject 里唯一的结构变化：**团短路的语义从「整段恒等」改成「本切片没有团」**。
原来 `if (切片在候选 z 范围外) return identity` 会连远景雾一起跳过；现在先算远景雾，
短路只跳团循环（远景雾是全局介质，本来每个 froxel 都有它）。

## 5. 水体契约：雾只在水上

`τ = τ_fog + τ_water` 只在两个介质**空间不相交**时精确（`jpov_water_contract.md` §1.3，
交叉衰减无闭式）。因此：

- **相机在水面以下 ⇒ 整块远景雾被忽略**（`Renderer` 直接不注入它）。
  这条写在 `DistanceFog` 的头注释里提醒使用者：**水下没有雾**。
- **有水体时，雾在水平面以下的那截被裁掉**：`Build(..., std::optional<float> fog_floor_y)`
  传水面高度，inject 里按半空间 `y ≥ floor` 裁剪段（裁剪后中点变了 ⇒ σ 按新中点重算，
  与采样器共用 `RoundBoxSigmaAt`）。
- 配置侧建议盒底压在水面上（y = 0）；即使没压，floor 裁剪也保证水下不重复计算。

## 6. 代价

- **唯一的开销变化**：以前只有点状雾团时整条 froxel 管线才跑；现在**只要开远景雾就跑**。
  量级 = `Nxy × nz` 个 texel 的 inject + reduce/scatter + 一次全屏合成。
  1280×720 / `tile_px=16` / `nz=256` ⇒ 3600 柱 × 256 ≈ **92 万 texel**。
  想省就调大 `tile_px`（耗时 ∝ `k²`，`k = √nz / tile_px`）或调小 `nz`。
- **远距精度**：网格只到 `FogParams::z_far`（默认 2000 m），更远的像素被 clamp 到
  `z_far` 处的累积值。对"雾在 2 km 内饱和"的场景这是**正确**行为（超过饱和距离本来就看不见）；
  要"一直不饱和的公里级尾巴"是级联体积的活，不在本 PR 范围。
- **横向分辨率**：`tile_px` 决定 band/盒边的平滑度（合成趟对 4 邻 tile 双线性）。
  16 px 在 720p 下够用；盒边本来就是软边，实测无台阶（见 §8）。

## 7. 刻意没做

- **方向性天光 / 太阳相位**：远景雾不吃光照，所以没有 god ray、没有迎光侧增亮。
  想要的话，正确做法是给它一份**低分辨率天光探针**（或让它走团那套 `Llight`），
  但那会把「体积消费者吃哪些光」的口径问题再引一遍 —— 单独 PR。
- **高度衰减（指数标高）**：当前 σ 在盒内是常数。真实雾是 `σ(y) = σ₀·e^{−(y−y₀)/H}`，
  地平线更厚是**路径长度**自然长出来的，不需要 band。这是下一步（同一处代码，加一个剖面）。
- **级联体积 / 远距解析尾巴**：见 §6。
- **折射射线消费雾**：水/SSR 目前不查 `FogField`。要接的话，`FogField` 就是唯一的那份数据
  （这也是把远景雾塞进场而不是再开一个 medium 的长期理由）。

## 8. 验收

`--capture-distance-fog`（`demo/jpov_skylight_viewer.cc`）出 8 张：

| 图 | 期望 |
|---|---|
| `fog_off` / `fog_on` | 近处几乎不变、远处与地平线变淡（空气透视）；无硬边、无条带 |
| `fog_thick`（σ×3） | 只是更浓，不引入新瑕疵 |
| `fog_linear`（kLinear） | 与指数剖面只在盒顶软边附近有差（内部 w=1 相同） |
| `fog_water_off/on` | 水皮/条带一起被雾影响，水下不泛白 |
| `fog_under_on/off` | **逐像素完全相同**（水下忽略） |

实测（1280×720，llvmpipe，默认参数 σ=0.008、层顶 24 m、软边 12 m）：
`off vs on` 100% 像素有变化、均值 13.4/255（雾是全场性的，边缘几乎不变）；
`on vs thick` 更大；`under on vs off` **diff = 0**（逐字节相同）。

单元/GPU 测试：

- `//tools/jpov/src/fog:fog_lower_test` —— `LowerDistanceFog` 字段映射 + 三条负向
  （硬边剖面 / σ=0 / 空盒）。
- `//tools/jpov/test/fog:distance_fog_gpu_test` —— 直接读回 scatter 纹理：
  均匀盒的 `τ_k = σ(z_{k+1}−z_near)`、`S = color(1−e^{−τ})`；无雾 ⇒ 场无效；
  薄板软边 ⇒ 壳区切片 `0 < w < 1` 且最深处 `w < 0.5`（**硬边会让这条红**）。

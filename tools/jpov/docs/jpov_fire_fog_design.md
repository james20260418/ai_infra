# Fire-Fog（雾火）— TPZ 统一体积雾火 · 最终设计

> 日期：2026-10-05 ｜ 线：`ai_infra_3` ｜ 状态：**最终设计（v1 待实现）**
> **2026-10-06 扩展**：v1 定调「**不做 3D 体积纹理、物理引擎铺开**」；补 §10「统一团模型 + 多类采样器 + box 容器」。
> **2026-10-10 接口升级**：源项拆**散射(albedo)/自发光(emission)**（去掉 intensity：emission 自带 HDR、sigma 自带 1/m）；
>   雾火**始终**走物理光照（ambient + 太阳×CSM + 点光源 tile culling，后者 fire_fog 自持一套 Nxy 网格光源表），
>   移除「无光照 / base 发射」链路（Danis：既然这么物理，不再提供非物理绘制）。
> **2026-10-11 采样器重构**：`uFogBodyTex` 改**定长 tagged union + 解码器 + 命名访问器**（§10.3）；
>   第一种 kind 改为 **`kRoundBox`（圆角盒，替换早期点雾/球）**；**求交移入采样函数**（OBB→TC 仍类型无关）；
>   采样器**严格 O(1)**；加**布局契约单测**防 CPU/GLSL 漂移。
> 模块：`tools/jpov/src/fire_fog/`（单一自包含 renderer；早期草案名 `point_fog` / `ball_fog`）
> 推演史 / 取舍理由见 [`jpov_tpz_volumetric_fog_design.md`](jpov_tpz_volumetric_fog_design.md)。**本文只讲定论。**

---

## 0. 一句话

**逐像素、无网格、不依赖 TAA** 的体积雾火：**屏幕 tile 剪枝 → 每像素深度函数（ZDist）→ 深度正确的屏幕空间高斯 → 末端积分**。

---

## 1. 核心模型：统一 `(T, S)` transfer 段

一切透明/体积元素 = 沿视线的一串段，每段是一个 transfer `(T, S)`：`T`=透射率、`S`=增量亮度(RGB)。

```
# 前到后合成
S_acc += T_acc · S_seg
T_acc *= T_seg
L_out  = L_scene · T_acc + S_acc
```

| 类 | 参数 | 传输 | 备注 |
|---|---|---|---|
| **thick medium** 雾/火/烟 | `L=z1−z0` | `T=exp(−σL)`；`S=c·(1−T)` | 体积，占 z 区间 |
| **thin surface** 玻璃片 / alpha 片 | — | `T=(1−F)·tint`；`S=F·L_env`（+`α·c`） | 表面，z 上 delta；`S` 可独立于 `T`（反射） |
| **thin refractive** 贵玻璃 | — | 非局部（**改方向**） | **TPZ 之前预绘，当实心**；不做嵌套折射 |

> 铁律：**生产者可分开（raymarch / 光栅 / 预绘），合成器必须唯一**（否则按材质分批 → 全局序错 → 穿帮）。

---

## 2. 三层「有界工作量」（TPZ）

```
L1  屏幕 tile 剪枝   → 每 tile 候选雾火 ≤ K = 16      （横向封顶）
L2  每像素 z 段       → 每像素参与合成 ≤ 8 控制点     （纵向封顶）
L3  深度正确屏幕高斯  → 只在 z 重叠处混合               （滤波封顶）
```

---

## 3. ZDist —— 逐像素深度函数（核心数据结构）

**两个单调不减的分段线性函数**，共享 ≤**8** 个控制点（含 `z_min`/`z_max`）：

| 符号 | 含义 | 通道 | 叠加 |
|---|---|---|---|
| `τd(z)` | **光学深度累积** `∫σ dz`（**不是透射率 T**） | 标量 | **加** |
| `Ed(z)` | **源累积** `∫E dz`，`E`=每长度内散射/发射亮度 | RGB×3 | **加** |

- ⚠️ **存 `τ`（可加、单调），不存 `T`（乘性）**。存 `T` 会退化成薄介质一阶近似（浓雾过亮）。
- **累积 = 加法 + 乱序**（顺序无关，免排序）。
- **两阶段**：FS 用**大 buffer 精确累加** → **降采样**到 ≤8（最小误差；`z_min`/`z_max` 恒保留）。避免链式漂移。
- **末端积分**：

```
τ_total = τd(z_min)
T_total = exp(−τ_total)
S_total = exp(+τd(z_min)) · ∫ exp(−τd(z)) dEd(z)     # 分段线性上的 Stieltjes 积分
L_out   = L_scene · T_total + S_total
```

- **玻璃/反射事件在末端插入**：透射面 = `τd` 一个**跳变**；反射/发光 = `Ed` 一个**台阶**。仍单调可加 ⇒ 与雾火同栈。

---

## 4. 采样

- 每个雾火实例按 **M 段**切弦（`M` **可配置**）。
- **分层抖动（stratified jitter）**：**M 均匀分割 + 段内抖动**（非整弦随机）⇒ 覆盖均匀、无驻波、杀结块。
- 段采样点查 **CSM** 得 `L_in` → 组成该段 `(σ, E)` 剖面 → **加法**叠进 ZDist。

---

## 5. 屏幕空间高斯（深度正确、函数域）

- 在**函数域**就地混合：中心 `ZDist0` 吸收邻居，**只在 z 重叠处**作用；邻居 range 不足处**按 ZDist0 补全**。
- **增量加权平均**（`ZDist0`=先验、`ZDist_i`=观测）：

```
w_i = 高斯权重 × z 重叠系数        # 无重叠跳过；重叠占 Z0 的 z-range 100% → 系数 1
a = Σ_{j<i} w_j ,  b = w_i
Z0f ← (a·Z0f + b·Ẑ_i) / (a + b)     # Ẑ_i = 邻居覆盖处取自身，未覆盖处取 Z0f
```

- **必须归一化**（否则能量随邻居数增长）。

---

## 6. 光照

- `L_in` = **sun · shadow(CSM)**（逐像素高频，出光柱） + **ambient + Σ点光**（每团每帧预烘）。
- 相位 v1 固定 `g=0`（各向同性）。

---

## 7. 明确不做 / 边界

- ❌ froxel；❌ 固定 z 网格（会带回格量化闪）。
- ❌ v1 不上 VSM（真闪再上**方差钳制版**，非裸 VSM；主阴影留 PCF）。
- ⏸ TAA 暂缓（控制点逐帧跳 ⇒ 历史不对应 ⇒ ghost，是独立子系统）。
- 🅿️ L3 屏幕高斯、玻璃、折射：v1/后续分期；贵玻璃折射**预绘、当实心**。

---

## 8. v1 范围与里程碑

- **v1 = L1 + L2**（+ ZDist）：tile 剪枝（cap 16）+ 每像素 z 段 + ZDist 累加 + 末端积分。
- 验收：**无 TAA 不闪 + 可 gold（确定）**。
- 顺序：先出**朴素版**基准，再把 **ZDist** 作为等价替换接上（有对照）。
- 埋点：L1 成员集跳变率、降采样误差、采样噪声收敛。

---

## 9. 参数速查

| 参数 | 值 | 归属 |
|---|---|---|
| `K`（tile 雾火上限） | 16 | JPOV 锁 |
| ZDist 控制点 | **8** | JPOV 锁 |
| `M`（每实例段数） | 可配（默认 3） | JPOV 锁 |
| tile 尺寸 | 16 / 32 | JPOV 锁 |
| 相位 `g` | 0 | JPOV 锁 |
| 用户可配 | 颜色 / 尺寸 / 强度 / 衰减类型 | 用户 |

---

## 10. 统一「团」模型 + 多类采样器 + box 容器（2026-10-06 扩展）

> v1 定调：**不做 3D 体积纹理，物理引擎铺开**。效果（火 / 烟 / 薄雾 / 浪花 / 冲击波 …）由**物理 / 程序化**产出，全部降维成**一种「团」**（`FogBody`）进同一条 tile culling + ZDist 管线；每个团的**采样器是「多种类」的**。

### 10.1 两层 + 唯一合成器

```
命令层（What —— 可分效果、带业务语义，可为不同子 command）
    FlameCmd / MistCmd / ShockwaveCmd / …      ← render command 的子 command
        │ 每帧 lower / flatten（各效果的产出逻辑住用户侧 / 命令层）
        ▼
后端团层（How —— 唯一表示）
    FogBody[N]                                  ← 一切效果降维成这一种「团」
        │ L1：屏幕 tile 剪枝（box 投影 → ≤K / 像素）
        ▼
每像素 ZDist（τd, Ed）累加 → 深度正确高斯 → 末端积分
```

- **铁律（承接 §1）**：生产者可分开（各子 command / raymarch / 光栅），**合成器必须唯一**。
  tile culling / ZDist / 末端积分**只认 `FogBody`**，不认任何业务名。

### 10.2 `FogBody` —— 后端唯一「团」定义

```cpp
// ── 容器：3D 空间 OBB（可旋转 box，普适容器）──
struct Obb {
    Vec3 center;       // 世界系中心
    Vec3 axis[3];      // 三正交单位轴（第 i 组；对齐效果自然轴：火/烟柱竖向、薄雾贴地）
    Vec3 half_extent;  // 三半轴长度（>0）；盒 = center ± Σ_i axis[i]·half_extent[i]
};

// ── 采样器种类（「多种类」的核心；可扩展）──
enum class FogFieldKind : uint8_t {
    kRoundBox        = 0,  // 圆角盒：OBB 容器 + 盒内按「圆角盒距离」衰减（替换早期点雾/球）
    kHeightSlab      = 1,  // 贴地垂直 slab + 2D 域扭曲滚动（地表薄雾）
    kNoiseVolume     = 2,  // fbm/curl 调制「密度 × 发射」（火 / 烟 / 蘑菇云）
    kShell           = 3,  // 薄壳（冲击波 / 爆燃环）
};

// ── 后端唯一团记录（POD；进缓冲纹理，texelFetch 随机访问）──
struct FogBody {
    Obb           bound;         // 容器（同时 = 积分区间 [t0,t1] 的界）
    FogFieldKind  kind;          // 采样器种类（唯一分派点）
    float         sigma;         // 消光系数 σ_t（1/m）
    Color         albedo;        // 单散射反照率 a = σ_s/σ_t ∈ [0,1]（散射色）
    Color         emission;      // 自发光系数 ε（HDR 辐射亮度/米）
    float         params[8];     // 采样器参数（超集，按 kind 解释；见 §10.3）
};
```

- 为何 `params[8]` 取超集而非逐 kind union（C++ 侧）：GL 3.3 + 确定性 + 单一缓冲纹理布局，性价比最高；kind 少时浪费可忽略。若某 kind 参数爆表，再拆二级 `params` buffer。
  **（2026-10-11）GPU 侧定案**：`uFogBodyTex` 改为**定长 tagged union**（公共头 + union 区，见 §10.3），GPU 侧不再自造“超集数组”，而是按 `d.kind` switch 到各自的 `SampleXXX`——零未用字段、可读性高。
- 源项 `J = albedo·L_light + ε/σ_t`（散射 + 自发光）；早期「衰减类型」菜单落到 `kRoundBox` 的 `d.atten`；**用户配 消光σ/散射色albedo/自发光emission/尺寸/衰减类型/圆角**，其余为 JPOV 锁定内部量。

### 10.3 采样器字段映射 + `uFogBodyTex` 布局契约（2026-10-11 定稿）

后端把每个团打包进一张 RGBA32F 一维条带纹理 `uFogBodyTex`（texelFetch 随机访问）。
**定长 tagged union**：每团占 `fog_layout::kTexelsPerBody = 10` 个 texel（stride 对所有 kind 一致）；
前 2 个 texel 是**公共头**（所有 kind 含义固定），其后是 **union 区**（按 kind 解释）。
**第一个字段 = kind**（类型判别子）。GLSL 侧写一个**解码器** `DecodeFogBody(idx) → FogBodyData d`
（“每团通用数据”）；公共层只经**命名访问器**（`FogCenter(d)` / `FogSigma(d)` …）读 `d`，禁止裸下标。

公共头（前 2 texel，所有 kind）：

| texel | x | y | z | w |
|---|---|---|---|---|
| 0 | `kind` | `sigma` | `albedo.r` | `albedo.g` |
| 1 | `albedo.b` | `emission.r` | `emission.g` | `emission.b` |

union 区（**按 kind 分作用域**，占同一段 texel、按 kind 解释）。当前两种 kind：

`kRoundBox`（texel 2..6）：

| texel | x | y | z | w |
|---|---|---|---|---|
| 2 | `center_x` | `center_y` | `center_z` | `bound_hx` |
| 3 | `bound_hy` | `bound_hz` | `axis0_x` | `axis0_y` |
| 4 | `axis0_z` | `axis1_x` | `axis1_y` | `axis1_z` |
| 5 | `axis2_x` | `axis2_y` | `axis2_z` | `radius` |
| 6 | `atten` | `reserved` | `reserved` | `reserved` |

`kFlame`（texel 2..4；燃料盒包裹燃烧，MVP）：

| texel | x | y | z | w |
|---|---|---|---|---|
| 2 | `center_x` | `center_y` | `center_z` | `box_hx` |
| 3 | `box_hy` | `box_hz` | `up_x` | `up_y` |
| 4 | `up_z` | `shell` | `plume` | `vigor` |

（texel 7..9 预留，供将来的复杂 kind —— 如噪声火。尺寸选值依据：最坏 kind 的解析参数 ≈ 36 float
+ 公共头 8 ⇒ 落在第 9 个 texel；故开 10、留一档。stride 是全局常量，改它动所有 kind，一次开够。）

**契约（防 CPU/GLSL 静默漂移）**：布局是两侧**单一来源** —— `fog_body.h` 的按作用域字段表
（`kFogBodyCommonLayout` 公共头 + `kRoundBoxUnionLayout` / `kFlameUnionLayout` 各 kind union；
C++ 表：字段名 + texel + 分量）与 `fire_fog_shader.h` 的 GLSL（`//@FOGLAYOUT <scope> ...` 标记行 +
`#define FOG_TEXELS_PER_BODY`）必须逐项一致。**契约单测** `fire_fog_contract_test.cc` 解析 GLSL
字符串里各 scope 的标记/宏与对应 C++ 表比对；改布局两处一起改，漏改即红。

> 注：CPU 侧打包器（`fire_fog_renderer.cc`）**不在契约单测覆盖内**（手写 texel 偏移）—— 加字段/
> 加 kind 时这里要人肉同步，是唯一需额外小心的裸点。

**求交也移入采样函数**：`OBB → TC`（CPU 剪枝）保持**类型无关**；而逐团的**光线求交与点求值都在
`SampleXXX` 里**（容器原语由 kind 定：`kRoundBox` = ray-OBB slab）。CPU 侧按 kind 构造容器
（`kRoundBox`：立方 OBB + 圆角）。

各采样器返回**统一的 `FogSegment{hit,t0,t1,p,sigma,albedo,emission}`**（“3 个返回项” = sigma /
albedo / emission）；主入口 `SampleFogSegment` 按 kind 分派（GLSL330 无函数指针 ⇒ if 链展开；
加 kind = 加一个分支，不动 L1 / L2 / ZDist）。

**采样器严格 O(1)**：`SampleXXX` 内**无循环、无迭代、无依赖纹理迭代**（fbm 若用，固定 octave 上限，
仍是 O(1)）；解码器只 fetch 该分支用到的 texel（stride 只是上界，热循环带宽不受连累）。

### 10.3.1 `kRoundBox` —— 第一种采样器（2026-10-11，10-11 晚修订）

**高自由度 box**：用户直接配 box 的**位置 + 长宽高**；box **外侧**再包一层厚度 = `radius` 的**球面圆角外壳**。

- 容器：OBB（轴 = 世界轴）；**容器半尺寸 = 内盒半 (size/2) + radius**（= 内盒 ⊕ 球 的外沿，剪枝 / z 范围保守不漏）。
  求交 = ray-OBB slab（容器）。
- 局部坐标 `q = Rᵀ(p−c)`（世界单位）。盒 SDF：`d = ‖max(|q|−half, 0)‖ + min(maxᵢ(|qᵢ|−halfᵢ), 0)`。
- **外壳归一化距离** `t`：盒内（`d ≤ 0`）→ `t = 0`（**无衰减，w = 1**）；盒外 `t = d/radius ∈ (0,1]`；`radius = 0` → 硬边盒。
- 消光 `σ = sigma · w(t)`，`w` = `FogAttenuation` 剖面（**只作用于圆角壳区**）。
- 用户可配：`PointFog{ center, size(长宽高), radius(外侧圆角壳厚度), sigma, albedo, emission, attenuation }`。

### 10.3.2 `kFlame` —— 燃料盒包裹燃烧「形」（2026-10-11，MVP：无 3D 纹理）

**一个团**实现「燃料盒被火焰包裹燃烧」的**几何形**（不含噪声细节，后续再加 3D 火纹理）。

- **域**：燃料盒（`center` + `size`，MVP 轴对齐）的 SDF；**盒内不着火**（盒 = 固体燃料，σ=0）。
- **包裹壳**：盒**表面外侧**厚 `shell` 的一层；`w_shell = 1 − d/shell`（`d` = 盒 SDF，`d ∈ (0, shell)`），
  密度从表面向外线性衰减。
- **浮力羽流**：盒顶沿 `up` 方向长 `plume` 的上升柱；半高 = 支撑函数 `Σ|upᵢ|·box_hᵢ`，柱半径随高度
  外扩（`max(box_h) + h`）、密度随高度衰减。`w = max(w_shell, w_plume)`。
- **容器 OBB**（求交 / CPU 剪枝）= 盒沿 `up` 外扩 `plume`、各向再外扩 `shell`（保守不漏）；CPU 与 GLSL
  用**同一公式**，燃料盒由容器 + `up`/`shell`/`plume` **反解**（同 kRoundBox 的 bound=外沿套路）。
- `σ = sigma · w`；`emission = emission_color · vigor · w`（`vigor` = 旺盛度，自发光放大）。
- 用户可配：`FlameFog{ center, size(长宽高), up(火苗方向), shell(壳厚), plume(羽流高), vigor(旺盛度),
  sigma, albedo, emission }`。
- **后续（待议）**：`up` 的**主+lag**双方向、`q → 共享 3D 火纹理 UV` 的映射、燃尽前沿等。

### 10.3.3 `kVolumeTex` —— 共享 3D 火纹理体积（2026-10-13）

**一个团**把一张**共享 3D 火纹理**（离线烘的 fire VDB：R=density、G=temperature，均 ∈[0,1]）按盒映射进体积管线。

- **域**：体积盒（OBB，MVP 轴对齐）；盒直接映射到纹理 `[0,1]³`（`uvw = q/(2·half) + 0.5`，盒外不采样 ⇒ 无场）。
- **采样**：`σ = sigma · density^density_pow`（`density_pow ≥ 1` 收薄）；`emission = emission_color · temperature · temp_scale`。
- **严格 O(1)**：求交 + 一次 `texture(sampler3D)`（无循环/无迭代）。
- 纹理**全局一张**：由 `FireFogRenderer::SetFireVolume(FireVolume)`（经 `Renderer::SetFireVolume` / `JPOV::SetFireVolume(path)`）上传；
  文件格式 `.jpovvol`（见 `fire_volume.h`），由离线转换器 `tools/jpov/tools/vdb_to_jpovvol.py`（pyopenvdb）从 VDB 生成。
- 用户可配：`VolumeFog{ center, size(长宽高), sigma, density_pow, temp_scale, albedo, emission }`。
- ⚠️ **已知**：froxel 管线每 tile 用**一条中心射线 + 每 z 段一次点采样** → 特写单个体积时横向分辨率≈tile_px，
  细节弱于逐像素 raymarch；换细 `tile_px` / 高 `nz` 可改善（llvmpipe 上更慢）。
  - ⚠️ **拉远断裂**：远处火焰占的 tile 变少，很多 tile 的中心射线打不到细火柱 → 中部空洞。
  - ⇒ **主角近景火**改用**逐像素 raymarch 渲染器**：设计与资产见 `/james_pm/fire_hero/`（`DESIGN.md`）。

### 10.4 统一采样契约 —— 唯一 kind 分支点

多类采样器**只在一个地方分叉**：`SampleFogSegment`（按 `d.kind` 分派）；`OBB→TC` 剪枝 / 分段 / 合成全
kind 无关。**光线求交也移入各 `SampleXXX`**（容器原语随 kind），但**返回结构与调用约定统一**：

```glsl
// 统一返回：命中段 [t0,t1]（已与本 froxel 段相交）+ 采样点 + 该点 (σ, albedo, emission)。
struct FogSegment { bool hit; float t0, t1; vec3 p; float sigma; vec3 albedo, emission; };

// 唯一 kind 相关：解码 d → 求交（容器随 kind）→ 段中点一次点求值。**严格 O(1)**。
FogSegment SampleRoundBox(FogBodyData d, vec3 ro, vec3 rd, float zk, float zk1, float znear);

// 唯一分派入口（GLSL330 无函数指针 ⇒ if 链）：
FogSegment SampleFogSegment(FogBodyData d, ...) {
    if (FogKind(d) == FOG_KIND_ROUNDBOX) { return SampleRoundBox(d, ...); }
    // 加 kind = 加一个分支
}

// —— 以下每条 kind 共享（inject 里；§3 / §4 已定）——
for (int j = 0; j < nf; ++j) {
    FogBodyData d = DecodeFogBody(fog_idx[j]);            // 解码器（§10.3）
    FogSegment seg = SampleFogSegment(d, ro, rd, zk, zk1, uZNear);
    if (!seg.hit) { continue; }
    float len = seg.t1 - seg.t0;
    float dtau = seg.sigma * len;
    if (dtau <= 0.0) { continue; }
    vec3 Llight = ambient + sun·CSM + Σ点光;              // 共享光照（§6）
    vec3 Sleaf = seg.albedo * Llight * (1 - exp(-dtau));
    Sleaf += seg.emission * (len * (dtau > 1e-6 ? (1 - exp(-dtau)) / dtau : 1.0));
    acc = OverCompose(acc, vec4(dtau, Sleaf));            // over 合成（团无序）
}
```

- **解码器 / 命名访问器 / 布局契约 / 严格 O(1** 见 §10.3。
- **加一个 kind 不动 L1 / L2 / ZDist**，只加一个 `SampleXXX` + `SampleFogSegment` 一个分支 ⇒ 这正是「团结构本身包含各类特效可能」的落地方式。
- **光照共享**：`L_in = ambient + sun·shadow(CSM) + Σ点光源`；各 kind 只决定「介质分量」怎么算。

### 10.5 容器 = 3D box（OBB）：紧密性 ↔ 剪枝算力

> Danis 关注：box 覆盖 3D 实体的**紧密性** vs tile culling 的**算力**。结论：**box 在这里几乎处处占优，唯一要处理的是近平面回退。**

- **越界的代价在「屏幕覆盖面积」，不在「每体变换」**。tile 剪枝成本 ≈ Σ 各体覆盖的 tile 数；包围越松 → 覆盖越大 → `(tile × 体)` 对越多。所以「越紧」直接省算力。
- **box 比球更紧（对各项异性效果）**：球半径 = 盒对角线 / 2；细长盒（火苗 / 烟柱 / 贴地薄雾）用球会**显著多覆盖**（越细长越夸张，常见 ~2–3× 面积）。⇒ box **既更通用、又更省剪枝**，这是它优于球的根因。
- **8 角投影 = 最紧 AABB 且 O(8)**：透视下凸多面体的像 = 其 8 个顶点像的**凸包** ⇒ 该凸包的屏幕 AABB 即**最紧的轴对齐界**，代价仅 8 次顶点变换（与点光源剪枝同阶）。
  - ⚠️ **任一角在近平面后（w ≤ 0；相机进入雾体 / 贴边）→ 投影失效 ⇒ 保守全屏覆盖**（与点光源 culling「球跨相机 → 全屏」同规则）。
- **再紧一档（可选，后话）**：对上述 AABB 内的候选 tile 做「**tile 矩形 vs 投影 2D 凸包**」SAT 测试 → 精确覆盖、去掉四角浪费。代价 = 每候选 tile 一次 SAT；AABB 浪费有界（最坏「角对相机」约 2×）。**默认先不上**。
- **盒内空洞由采样器吸收**：box ⊇ 场（如泪滴形火苗的四角无场）⇒ 该处 `σ≈0` ⇒ ZDist 累加为空 ⇒「基本区间合并」自动坍缩，成本只是几次 `SampleField`。缓解：① 盒 = 场的**最紧 OBB**（允许旋转对齐效果轴）；② 采样器对盒外 / `|u_local|>1` 早退；③（后话）per-body `inner_scale` 给出更紧的积分区间。
- **一个界管两件事**：box 同时给出**积分区间 `[t0,t1]`**（ray-OBB slab）⇒ 盒越紧、采样段越短。**紧密性对 culling 与积分同向增益**。

**定论**：`OBB + 8 角投影 AABB → 覆盖 tile`，**近平面保守全屏回退**；精确凸包覆盖留作后续优化。

### 10.6 里程碑补充（承接 §8）

- **M1a**：模块骨架（`tools/jpov/src/fire_fog/`）+ `FogBody` / `Obb` / `FogFieldKind` + **单一 kind `kRoundBox`** + box L1 剪枝（8 角投影）+ ZDist 累加 + 末端积分 + **无 TAA gold**。
- **M1b**：依次加 `kNoiseVolume`（火焰）、`kHeightSlab`（贴地薄雾）、`kShell`（冲击波）——**每加一个不动 L1 / L2 / ZDist**。
- 验收（承 §8）：无 TAA 不闪 + 可 gold（确定）。

---

## 11. ZDist 的 CPU 参考实现 + 纹理布局（2026-10-08）

> 代码：`tools/jpov/src/fire_fog/zdist_function.h` + `zdist_texture_layout.h`（**GL-free、可单测**，12 用例）。
> 思路（Danis）：**先把 shader 要做的这套操作在 CPU 上实现一遍**，作为 shader 的对照 / gold / debug。
> 本 PR 只做 CPU 侧（M1a 的一个前哨），不动 GPU pass。

### 11.1 约定（camera-forward）

- 自变量 `z` = **线性视深**（米），自相机向场景递增；函数域 = `[z_near, z_far]`。
- `τd(z)` = 相机→z 的**光学深度** `∫σ dz`（标量，单调不减；`τd(z_near)=0`）。
- `Ed(z)` = 源累积 `∫E dz`（RGB，各分量单调不减；`Ed(z_near)=0`）。
- ⚠️ **存 `τ`（可加）不存 `T`（乘性）**——否则退化成薄介质一阶近似（浓雾过亮）。
- 末端积分：`τ_total = τd(z_far)`；`T = exp(−τ_total)`；`S = ∫ exp(−τd) dEd`；`L_out = L_scene·T + S`。
- 与 §3「z 自墙深累积到最近」是**同一积分的反向记法**，数学等价（这里取相机正向，读起来更直接）。

### 11.2 叠加一个 z 段（= 上升区间 + 守恒）

一个 segment `[z0,z1]`（消光 `σ`、内散射 `L_in`）对 ZDist 的贡献是**两条 ramp**：

- `τd` 上升 `σ·L`（`L = z1 − z0`）；`Ed` 上升 `σ·L_in·L`。ramp 在 `z0` 前为 0、`z1` 后为平台；两者共享断点 `z0/z1`。
- 实现：先把 `z0/z1` 变成断点（用**当前**精确函数值），再**对所有断点加上该 ramp 值**。
  ⇒ 结果是各 ramp 之和 ⇒ **加法、乱序无关**（免排序，正是 §3 要的性质）。
- 段超出域 → 裁；与域无交叠 → 无操作。

### 11.3 降维到 8 控制点（守恒）

- **两阶段**（§3）：精确累加（控制点数不限）→ 降采样到 ≤8。
- **「总上升量守恒」落在一条不变量上：端点 `z_near/z_far` 恒保留。**
  ⇒ `τ_total` / `Ed_total` 端点值逐位不变 ⇒ `T_total` 精确，误差只落在中间、只影响 `S`。
- 算法：候选 `z` = 累加器全部断点；**DP 选 8 个（含两端）**，最小化**以 `exp(−τd)` 加权**的四通道 L2 误差
  —— 权重即「该处对末端源积分 `S` 的贡献」⇒ 误差度量对**最终 exp 积分**负责，而非函数 L2（对齐 §3 的评审点 2）。
- shader 跑不动 DP（`O(K·m²)`）：GPU 侧只能上更便宜的启发式；**本 CPU 版即该启发式的真值 / 对照**。

### 11.4 末端积分（Stieltjes，闭式）

分段线性函数上，源积分逐段闭式：`S = Σ_seg ΔEd · exp(−τd_i) · E1(Δτ)`，其中 `E1(x) = (1−e^{−x})/x`（`x→0` 取极限 1）。
单段均匀雾时退化为 `S = c·(1−T)`，与 §1 的表一致（已单测）。

### 11.5 纹理布局（定稿：6 个 RGBA32F texel / 像素）

- 每控制点 5 个量：`(z, τd, Ed.rgb)`。**8 个 `z` 被四条通道共享**（同一组断点）⇒ `z` 只存一份。
  - 若不共享：8 点 ×(z,值) ×4 通道 = 64 float = **16 个 RGBA32F**（= Danis 担心的「16 个纹理」的由来）。
- 再按 §11.6 的精度结论做**混合精度 + 位打包** ⇒ **6 个 RGBA32F texel = 24 个 32-bit lane / 像素（96 字节）**：
  - `lane[0,8)`   : `z` **fp32**（2 texel）
  - `lane[8,12)`  : `τd` **uint16**（量程 [0, 32]，2/lane，1 texel）
  - `lane[12,24)` : `Ed.rgb` **fp16**（2/lane，3 texel）
- 打包/解包：`PackZDist` / `UnpackZDist`（含便携 float↔half 位转换，不依赖 `_Float16`，兼容 MinGW）。

### 11.6 精度预算（实测，500 组随机 ZDist）

各通道进末端积分的方式不同 ⇒ 该省的省、该留的留：

| 通道 | 进积分方式 | 结论 |
|---|---|---|
| `z` | **不进颜色积分**（闭式末端积分只用 τ、ΔEd）；它决定断点间隙分辨 + 与场景深度对齐 | **必须 fp32** |
| `τd` | 进指数（`T=exp(−τ)`、`exp(−τd)` 权重）⇒ 误差被放大 | **uint16 unorm**（要**绝对**精度）|
| `Ed` | **线性**进 `S` | **fp16**（相对精度足够）|

- 关键：`T` 的相对误差 = `τ` 的**绝对**误差 ⇒ 均匀**绝对**步长的 unorm16 明显优于均匀**相对**步长的 fp16/bf16：
  - `τ→fp16`：T 相对误差 mean 1.1e-3 / max 3.9e-3；`τ→bf16`：1–3%（只 8 位尾数，差）；
  - **`τ→unorm16[0,32]`：≈ 无损**（T 误差 <5e-4，步长 32/65535≈4.9e-4）。
- `z→fp16/bf16` 对颜色**零影响**（实测 = 基线，因为末端积分不含 z）——但会并断点、破坏几何对齐，故仍留 fp32。
- `Ed→fp16`：相对 ~1e-4（可忽略）；`Ed→bf16` ~0.2%（也可）。

### 11.7 降维：DP（最优）vs 贪心（GPU 友好）+ 前缀和 O(1) 代价

- **前缀和 O(1) 代价**（`zdist_detail::PrefixCost`）：`cost(p,q)` 展开成**两端点值的二次型**（系数 = 6 条区间和），
  建表 O(m) 后任意 `(p,q)` **O(1)**。单测与现算 `SegmentCost` 逐值一致。
- **贪心降维**（`GreedyReduce`）：反复**合并掉“增量误差最小”的相邻点**到 ≤8。
  - GPU 友好：定长数组 + **前后指针链表**（**无二分**、无堆、无递归、无堆分配）；每步“找最小”是
    **线性扫描（GPU 上可并行归约）**。
  - 复杂度：**O(m·(m−K)) work**；两端点恒保留 ⇒ 总上升量守恒（与 DP 同）。
- **DP（`Reduce`）仍是最优真值**（§11.3）；贪心是它的启发式代理。

**误差 / 速度实测**（每档 300 组，z∈[0,40]，τ_total 到 ~10）：

| 候选 m | cost(贪心)/cost(DP) 均值 (max) | S 误差 DP → 贪心 | T 误差 | DP 用时 | 贪心用时 |
|---|---|---|---|---|---|
| 14 | 1.15 (2.4) | 1.3% → 1.5% | 0 → 0 | | |
| 22 | 1.25 (2.2) | 1.8% → 2.0% | 0 → 0 | | |
| 30 | 1.27 (3.8) | 1.7% → 1.9% | 0 → 0 | | |
| 42 | 1.31 (2.6) | 1.4% → 1.6% | 0 → 0 | | |
| 62 | 1.34 (2.8) | 1.2% → 1.5% | 0 → 0 | 5.8 ms | 72 µs |
| 122 | — | — | — | 47 ms | 314 µs |

- **`GreedyReduce`**（扫描版，GPU 友好）：每步 O(m) 线性扫描选最小 ⇒ **O(m·(m−K))**。
- **`GreedyReduceFast`**（二叉最小堆版，**同一贪心、结果逐位相同**）：取最小 O(1) + 更新 O(log m)
  ⇒ **O(m log m)**；m≲256 时 log2(m) ≤ 8 = K ⇒ **实际 O(m·K)**。无需二分搜索（链表定位邻居）、
  无 STL 容器、无动态分配。（堆的数据依赖分支不适配 GPU 的 SIMD 风格 ⇒ GPU 侧用扫描版。）

⇒ 贪心代价比最优高 **~15–35%**（最坏 2–4×）；**S 误差只比 DP 差 ~10–20%**；T 精确。
速度（单线程 CPU，m 越大差距越大）：

| 候选 m | DP | 扫描贪心 | 堆贪心 | 堆/DP |
|---|---|---|---|---|
| 62 | 6.3 ms | 143 µs | **40 µs** | 157× |
| 122 | 48 ms | 310 µs | **42 µs** | 1148× |
| 242 | 329 ms | 1133 µs | **101 µs** | 3272× |

⚠️ 即便堆版 O(m log m)≈O(m·K)，**每像素**跑 ~百个 m 仍是几千次带分支操作 —— shader 里可能还需限制 m（或换更粗的在线合并）；CPU 这两版当“真值 / 近优”参照。

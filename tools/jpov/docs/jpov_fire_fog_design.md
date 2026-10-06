# Fire-Fog（雾火）— TPZ 统一体积雾火 · 最终设计

> 日期：2026-10-05 ｜ 线：`ai_infra_3` ｜ 状态：**最终设计（v1 待实现）**
> **2026-10-06 扩展**：v1 定调「**不做 3D 体积纹理、物理引擎铺开**」；补 §10「统一团模型 + 多类采样器 + box 容器」。
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
    kAnalyticProfile = 0,  // 闭式衰减剖面（承接 kDome/kSharp/kUniform/kCoreRamp）
    kHeightSlab      = 1,  // 贴地垂直 slab + 2D 域扭曲滚动（地表薄雾）
    kNoiseVolume     = 2,  // fbm/curl 调制「密度 × 发射」（火 / 烟 / 蘑菇云）
    kShell           = 3,  // 薄壳（冲击波 / 爆燃环）
};

// ── 后端唯一团记录（POD；进缓冲纹理，texelFetch 随机访问）──
struct FogBody {
    Obb           bound;         // 容器（同时 = 积分区间 [t0,t1] 的界）
    FogFieldKind  kind;          // 采样器种类（唯一分派点）
    Vec3          color;         // 介质 / 发射色（HDR，可 > 1）
    float         intensity;     // 总体强度（HDR）
    float         sigma_scale;   // 消光尺度（1/m）
    float         params[8];     // 采样器参数（超集，按 kind 解释；见 §10.3）
    Vec3          prebaked_lin;  // 每帧预烘慢变项：ambient + Σ点光（团心）
};
```

- 为何 `params[8]` 取超集而非 union：GL 3.3 + 确定性 + 单一缓冲纹理布局，性价比最高；kind 少时浪费可忽略。若某 kind 参数爆表，再拆二级 `params` buffer。
- 早期「衰减类型」菜单（§9「用户可配：衰减类型」）落到 `kAnalyticProfile` 的 `params[0]`；**用户仍只配 颜色/尺寸/强度/衰减类型**，其余为 JPOV 锁定内部量。

### 10.3 各采样器的字段映射（示意，实现时定稿）

| kind | 语义 | `params` 映射（示意） |
|---|---|---|
| `kAnalyticProfile` | 闭式剖面 | `[0]`=profile 枚举，`[1]`=内部形状尺度，`[2..]` 预留 |
| `kHeightSlab` | 贴地薄雾 | `[0]`底高 `[1]`厚度 `[2]`垂直衰减形态 `[3]`noise 频率 `[4]`滚动速度 `[5]`风向方位角 |
| `kNoiseVolume` | 火 / 烟 | `[0]`noise 频率 `[1]`fbm 倍频 `[2]`锐度/阈值 `[3]`竖向拉伸 `[4]`湍流强度 `[5]`色带参数 |
| `kShell` | 冲击波 | `[0]`半径 `[1]`壳厚 `[2]`膨胀相位 |

### 10.4 统一采样契约 —— 唯一 kind 分支点

多类采样器**只在「点求值」处分叉**；几何 / 采样 / 合成全部 kind 无关：

```glsl
// 唯一 kind 相关：点求值。返回该点 L_in 的介质分量（RGB），并写出消光 σ。
vec3 SampleField(int kind, FogBody b, vec3 p_world, out float sigma);

// —— 以下每条 kind 共享（§3 / §4 已定）——
for (int j = 0; j < tile_count; ++j) {
    FogBody b = bodies[tile[j]];
    vec2 tt = RayObb(b.bound, ray);                        // slab 求交 → 区间 [t0,t1]
    if (tt.y <= tt.x) {
        continue;
    }
    for (int k = 0; k < M; ++k) {
        float t  = tt.x + (float(k) + Jitter(px, j, k)) * (tt.y - tt.x) / float(M);
        vec3  p  = ray.o + t * ray.d;
        float sigma;
        vec3  emis = SampleField(b.kind, b, p, sigma);
        if (sigma <= kSigmaEps) {
            continue;                                      // 空场早退
        }
        vec3 L_in = emis + b.prebaked_lin + SunShadowCSM(p);   // 共享光照（§6）
        ZDistAccumulate(/*dτ=*/sigma * dseg,               // 加法叠进 (τd, Ed)
                        /*dE=*/sigma * L_in * dseg);
    }
}
```

- **`M` 段、分层抖动、CSM、ZDist 累加 = 全 kind 共享**（§3 / §4）。
- **加一个 kind 不动 L1 / L2 / ZDist**，只加一个 `SampleField` 分支 ⇒ 这正是「团结构本身包含各类特效可能」的落地方式。
- **光照也共享**：`L_in = 介质发射(emis) + 慢变预烘(prebaked_lin) + sun·shadow(CSM)`。各 kind 只决定「介质分量」怎么算。

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

- **M1a**：模块骨架（`tools/jpov/src/fire_fog/`）+ `FogBody` / `Obb` / `FogFieldKind` + **单一 kind `kAnalyticProfile`** + box L1 剪枝（8 角投影）+ ZDist 累加 + 末端积分 + **无 TAA gold**。
- **M1b**：依次加 `kNoiseVolume`（火焰）、`kHeightSlab`（贴地薄雾）、`kShell`（冲击波）——**每加一个不动 L1 / L2 / ZDist**。
- 验收（承 §8）：无 TAA 不闪 + 可 gold（确定）。
